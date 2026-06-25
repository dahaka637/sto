#include "core/RuntimeCheck.hpp"

#include "imgui.h"

#include <Windows.h>
#include <cstring>
#include <stdlib.h>

namespace sto::runtime {
namespace {

struct RuntimeIssue {
    char component[64];
    char description[256];
};

// 0 = pending, 1 = ok, 2 = issues
// Written once by the worker thread (InterlockedExchange), read from the main thread.
static volatile LONG g_state = 0;

// SRWLOCK_INIT is a compile-time zero constant — no constructor, no MSVCP140.dll.
static SRWLOCK g_srw = SRWLOCK_INIT;
static RuntimeIssue g_issues[8];
static int g_issue_count = 0;

static bool g_popup_shown = false;
static bool g_dismissed   = false;

bool file_exists_win32(const wchar_t* dir, const wchar_t* rel) {
    wchar_t full[MAX_PATH]{};
    wcsncpy_s(full, _countof(full), dir, _TRUNCATE);
    wcsncat_s(full, _countof(full), rel, _TRUNCATE);
    return GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES;
}

void run_checks() {
    // Entire function is free of MSVCP140.dll calls:
    // no std::string, no std::vector, no std::mutex — only Win32 + UCRT.

    wchar_t exe_dir[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exe_dir, _countof(exe_dir));
    if (len == 0 || len >= _countof(exe_dir)) {
        AcquireSRWLockExclusive(&g_srw);
        g_issue_count = 1;
        strcpy_s(g_issues[0].component, "Sistema");
        strcpy_s(g_issues[0].description,
            "Nao foi possivel determinar o diretorio do executavel.");
        ReleaseSRWLockExclusive(&g_srw);
        InterlockedExchange(&g_state, 2L);
        return;
    }
    wchar_t* last_sep = wcsrchr(exe_dir, L'\\');
    if (last_sep) *(last_sep + 1) = L'\0';

    struct FileCheck {
        const wchar_t* rel_path;
        const char*    component;
        const char*    description;
    };
    static const FileCheck checks[] = {
        {L"runtime\\ffmpeg\\ffmpeg.exe",
            "FFmpeg",         "Conversor de audio/video (ffmpeg.exe) nao encontrado."},
        {L"runtime\\ffmpeg\\ffprobe.exe",
            "FFprobe",        "Analisador de midia (ffprobe.exe) nao encontrado."},
        {L"runtime\\ghostscript\\gswin64c.exe",
            "Ghostscript",    "Processador de PDF (gswin64c.exe) nao encontrado."},
        {L"runtime\\whisper\\whisper-cli.exe",
            "Whisper CLI",    "Motor de transcricao (whisper-cli.exe) nao encontrado."},
        {L"runtime\\whisper\\ggml-large-v3-turbo.bin",
            "Modelo Whisper", "Modelo de IA (ggml-large-v3-turbo.bin) nao encontrado."},
        {L"runtime\\pdf2docx\\pdf2docx.exe",
            "PDF para Word",  "Conversor de PDF para DOCX (pdf2docx.exe) nao encontrado."},
    };

    RuntimeIssue local_issues[8]{};
    int local_count = 0;
    for (const auto& c : checks) {
        if (local_count >= 8) break;
        if (!file_exists_win32(exe_dir, c.rel_path)) {
            strcpy_s(local_issues[local_count].component,   c.component);
            strcpy_s(local_issues[local_count].description, c.description);
            ++local_count;
        }
    }

    AcquireSRWLockExclusive(&g_srw);
    g_issue_count = local_count;
    for (int i = 0; i < local_count; ++i) g_issues[i] = local_issues[i];
    ReleaseSRWLockExclusive(&g_srw);
    InterlockedExchange(&g_state, local_count == 0 ? 1L : 2L);
}

// Pure Win32 thread proc — no std::thread, no MSVCP140.dll _LaunchPad machinery.
static DWORD WINAPI run_checks_thread_proc(LPVOID) {
    run_checks();
    return 0;
}

} // namespace

void start_check() {
    static bool started = false;
    if (started) return;
    started = true;
    const HANDLE t = CreateThread(nullptr, 0, run_checks_thread_proc, nullptr, 0, nullptr);
    if (t != nullptr) CloseHandle(t);
}

void render_if_needed() {
    if (g_dismissed) return;
    if (g_state != 2L) return;

    if (!g_popup_shown) {
        g_popup_shown = true;
        ImGui::OpenPopup("##sto-runtime-check");
    }

    constexpr ImVec4 kYellow = {1.00F, 0.76F, 0.03F, 1.00F};
    constexpr ImVec4 kRed    = {1.00F, 0.22F, 0.22F, 1.00F};
    constexpr ImVec4 kMuted  = {0.52F, 0.55F, 0.58F, 1.00F};
    constexpr ImVec4 kWhite  = {0.94F, 0.95F, 0.96F, 1.00F};

    ImGui::SetNextWindowSize({560.0F, 0.0F}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImGui::GetMainViewport()->GetCenter(),
        ImGuiCond_Appearing,
        {0.5F, 0.5F}
    );
    if (!ImGui::BeginPopupModal("##sto-runtime-check", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextColored(kYellow, "  Dependencias ausentes detectadas");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "O STO verificou que alguns componentes de execucao estao ausentes. "
        "Determinadas funcionalidades poderao nao operar corretamente."
    );
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Copy under shared lock before rendering (no std::vector, no heap).
    RuntimeIssue local_issues[8];
    int local_count;
    AcquireSRWLockShared(&g_srw);
    local_count = g_issue_count;
    for (int i = 0; i < local_count && i < 8; ++i) local_issues[i] = g_issues[i];
    ReleaseSRWLockShared(&g_srw);

    for (int i = 0; i < local_count; ++i) {
        ImGui::TextColored(kRed, "  %s", local_issues[i].component);
        ImGui::SameLine(150.0F);
        ImGui::TextColored(kWhite, "%s", local_issues[i].description);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(kMuted,
        "Verifique se todos os arquivos do release foram copiados corretamente para esta pasta.");
    ImGui::Spacing();

    if (ImGui::Button("Entendido", {-1.0F, 36.0F})) {
        g_dismissed = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace sto::runtime
