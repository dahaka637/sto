#include "CrashReporter.hpp"
#include "core/files/FileUtils.hpp"

#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>

#include <array>
#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace sto::crash {
namespace {

// ---- Helpers ----

std::string current_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string filename_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm);
    return buf;
}

std::string windows_version() {
    OSVERSIONINFOEXW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);
    // RtlGetVersion bypasses the compat shim — GetVersionEx lies on Win10+
    // Declared via void* to avoid pulling in <winternl.h> for PRTL_OSVERSIONINFOW
    using FnRtlGetVersion = LONG (WINAPI*)(void*);
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        const auto fn = reinterpret_cast<FnRtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) fn(&osvi);
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Windows %lu.%lu (Build %lu)",
        osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
    return buf;
}

std::string seh_code_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:        return "Violacao de acesso (0xC0000005)";
    case EXCEPTION_STACK_OVERFLOW:          return "Stack overflow (0xC00000FD)";
    case EXCEPTION_ILLEGAL_INSTRUCTION:     return "Instrucao ilegal (0xC000001D)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "Divisao inteira por zero (0xC0000094)";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "Divisao ponto-flutuante por zero (0xC000008E)";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "Array fora dos limites (0xC000008C)";
    case EXCEPTION_DATATYPE_MISALIGNMENT:   return "Desalinhamento de dado (0x80000002)";
    case EXCEPTION_BREAKPOINT:              return "Breakpoint (0x80000003)";
    case 0xE06D7363UL:                      return "Excecao C++ nao capturada (0xE06D7363)";
    default: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Desconhecido (0x%08lX)", code);
        return buf;
    }
    }
}

// ---- Stack trace ----
// ctx is passed by value — StackWalk64 modifies the CONTEXT as it unwinds.

std::string stack_trace(CONTEXT ctx) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();

    STACKFRAME64 frame{};
    frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

#ifdef _M_X64
    constexpr DWORD kMachine    = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#else
    constexpr DWORD kMachine    = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = ctx.Eip;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrStack.Offset = ctx.Esp;
#endif

    constexpr int kSymBufSize = sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(CHAR);
    std::array<char, kSymBufSize> sym_buf{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf.data());
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = MAX_SYM_NAME;

    std::ostringstream oss;
    constexpr int kMaxFrames = 48;

    for (int i = 0; i < kMaxFrames; ++i) {
        if (!StackWalk64(kMachine, process, thread, &frame, &ctx,
                nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        if (frame.AddrPC.Offset == 0) break;

        const DWORD64 addr = frame.AddrPC.Offset;
        DWORD64 sym_disp = 0;
        char line[640];

        if (SymFromAddr(process, addr, &sym_disp, sym)) {
            IMAGEHLP_LINE64 src{sizeof(IMAGEHLP_LINE64)};
            DWORD line_disp = 0;
            if (SymGetLineFromAddr64(process, addr, &line_disp, &src)) {
                const char* fname = src.FileName;
                if (const char* p = std::strrchr(fname, '\\')) fname = p + 1;
                std::snprintf(line, sizeof(line),
                    "  #%-2d  %-52s +0x%04llX  [%s:%lu]\n",
                    i, sym->Name, sym_disp, fname, src.LineNumber);
            } else {
                std::snprintf(line, sizeof(line),
                    "  #%-2d  %-52s +0x%04llX\n",
                    i, sym->Name, sym_disp);
            }
        } else {
            // No symbol: emit module + RVA so the developer can look it up in the PDB
            HMODULE hmod = nullptr;
            char mod_path[MAX_PATH]{};
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(addr), &hmod) && hmod) {
                GetModuleFileNameA(hmod, mod_path, MAX_PATH);
                MODULEINFO mi{};
                GetModuleInformation(process, hmod, &mi, sizeof(mi));
                const auto base = reinterpret_cast<DWORD64>(mi.lpBaseOfDll);
                const char* mname = mod_path;
                if (const char* p = std::strrchr(mod_path, '\\')) mname = p + 1;
                std::snprintf(line, sizeof(line),
                    "  #%-2d  %s +0x%llX\n", i, mname, addr - base);
            } else {
                std::snprintf(line, sizeof(line),
                    "  #%-2d  0x%016llX\n", i, addr);
            }
        }
        oss << line;
    }
    return oss.str();
}

std::string module_list() {
    HANDLE process = GetCurrentProcess();
    std::array<HMODULE, 512> mods{};
    DWORD needed = 0;
    if (!EnumProcessModules(process, mods.data(),
            static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) {
        return "  (EnumProcessModules falhou)\n";
    }
    const DWORD count = std::min(
        needed / static_cast<DWORD>(sizeof(HMODULE)),
        static_cast<DWORD>(mods.size()));

    std::ostringstream oss;
    for (DWORD i = 0; i < count; ++i) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(mods[i], path, MAX_PATH);
        MODULEINFO mi{};
        GetModuleInformation(process, mods[i], &mi, sizeof(mi));
        char ln[MAX_PATH + 64];
        std::snprintf(ln, sizeof(ln), "  0x%016llX  %s\n",
            reinterpret_cast<DWORD64>(mi.lpBaseOfDll), path);
        oss << ln;
    }
    return oss.str();
}

void write_report(const std::string& content) {
    try {
        const auto dir = sto::files::executable_directory() / L"crash_reports";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return;

        const auto path = dir / (filename_timestamp() + ".txt");
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (out.is_open()) out << content;
    } catch (...) {}
}

std::string build_header() {
    std::ostringstream h;
    h << "====================================================\n";
    h << " STO  -  Relatorio de Falha Critica (Crash Report)\n";
    h << "====================================================\n\n";
    h << "Data/hora   : " << current_timestamp() << "\n";
    h << "Versao STO  : 1.0.0\n";
    h << "Sistema     : " << windows_version() << "\n";
    try {
        h << "Executavel  : "
          << (sto::files::executable_directory() / L"STO.exe").string() << "\n";
    } catch (...) {}
    h << "\n";
    return h.str();
}

// ---- Handlers ----

LONG WINAPI on_seh_exception(EXCEPTION_POINTERS* ep) {
    static bool entered = false;
    if (entered) return EXCEPTION_CONTINUE_SEARCH;
    entered = true;

    const DWORD  code = ep->ExceptionRecord->ExceptionCode;
    const DWORD64 eaddr = reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress);

    std::ostringstream report;
    report << build_header();

    report << "=== Tipo de Falha ===\n";
    report << "Categoria : Excecao de sistema (SEH / Windows structured exception)\n";
    report << "Codigo    : " << seh_code_name(code) << "\n";
    char addr_hex[24];
    std::snprintf(addr_hex, sizeof(addr_hex), "0x%016llX", eaddr);
    report << "Endereco  : " << addr_hex << "\n";

    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const bool is_write = ep->ExceptionRecord->ExceptionInformation[0] != 0;
        char av_hex[24];
        std::snprintf(av_hex, sizeof(av_hex), "0x%016llX",
            ep->ExceptionRecord->ExceptionInformation[1]);
        report << "AV tipo   : " << (is_write ? "Escrita" : "Leitura")
               << " invalida em " << av_hex << "\n";
    }
    report << "\n";

    if (code == EXCEPTION_STACK_OVERFLOW) {
        report << "=== Pilha de Chamadas ===\n";
        report << "  (indisponivel — stack overflow esgotou toda a pilha de execucao)\n";
    } else {
        report << "=== Pilha de Chamadas ===\n";
        report << stack_trace(*ep->ContextRecord);
    }

    report << "\n=== Modulos Carregados ===\n";
    report << module_list();

    write_report(report.str());

    return EXCEPTION_CONTINUE_SEARCH;
}

void on_terminate() {
    static bool entered = false;
    if (entered) { ::abort(); }
    entered = true;

    std::ostringstream report;
    report << build_header();

    report << "=== Tipo de Falha ===\n";
    report << "Categoria : C++ std::terminate() chamado\n";
    report << "Causa tipica: excecao nao capturada em thread separado,\n";
    report << "              violacao de especificacao noexcept, ou destrutor lancando.\n\n";

    report << "=== Detalhes da Excecao Ativa ===\n";
    const auto current_ex = std::current_exception();
    if (current_ex) {
        try {
            std::rethrow_exception(current_ex);
        } catch (const std::filesystem::filesystem_error& ex) {
            report << "Classe   : std::filesystem::filesystem_error\n";
            report << "Mensagem : " << ex.what() << "\n";
            report << "Codigo   : " << ex.code().value()
                   << " (" << ex.code().message() << ")\n";
            if (!ex.path1().empty())
                report << "Caminho  : " << ex.path1().string() << "\n";
        } catch (const std::system_error& ex) {
            report << "Classe   : std::system_error\n";
            report << "Mensagem : " << ex.what() << "\n";
            report << "Codigo   : " << ex.code().value()
                   << " (" << ex.code().message() << ")\n";
        } catch (const std::bad_alloc& ex) {
            report << "Classe   : std::bad_alloc\n";
            report << "Mensagem : " << ex.what() << "\n";
        } catch (const std::exception& ex) {
            report << "Classe   : std::exception (ou derivada)\n";
            report << "Mensagem : " << ex.what() << "\n";
        } catch (...) {
            report << "Tipo     : excecao nao-padrao (catch(...))\n";
        }
    } else {
        report << "Nenhuma excecao C++ ativa no momento do terminate.\n";
        report << "Possivel causa: abort() direto, destrutor lancando, ou thread sem handler.\n";
    }
    report << "\n";

    // RtlCaptureContext captures this thread's current execution context.
    // The stack trace will show the path from the crashing thread to terminate().
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&ctx);

    report << "=== Pilha de Chamadas (contexto do terminate) ===\n";
    report << stack_trace(ctx);

    report << "\n=== Modulos Carregados ===\n";
    report << module_list();

    write_report(report.str());

    ::abort();
}

} // namespace

void install() {
    // Initialize symbol resolution early so modules loaded later are covered
    // (SYMOPT_DEFERRED_LOADS causes lazy resolution on first SymFromAddr call)
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    SetUnhandledExceptionFilter(on_seh_exception);
    std::set_terminate(on_terminate);
}

} // namespace sto::crash
