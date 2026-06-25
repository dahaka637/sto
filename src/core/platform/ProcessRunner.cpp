#include "core/platform/ProcessRunner.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <thread>

namespace sto::platform {
namespace {
std::wstring quote(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    for (const wchar_t character : value) {
        if (character == L'"') result += L'\\';
        result += character;
    }
    return result + L"\"";
}
}

ProcessResult run_process(const std::vector<std::wstring>& arguments, std::atomic_bool& cancel_requested, const std::function<void(const std::string&)>& on_line, const std::wstring& working_directory) {
    ProcessResult result;
    if (arguments.empty()) return result;

    std::wstring command_line;
    for (const auto& argument : arguments) {
        if (!command_line.empty()) command_line += L' ';
        command_line += quote(argument);
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return result;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> writable(command_line.begin(), command_line.end());
    writable.push_back(L'\0');

    const wchar_t* work_dir = working_directory.empty() ? nullptr : working_directory.c_str();
    if (!CreateProcessW(nullptr, writable.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, work_dir, &startup, &process)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return result;
    }
    result.started = true;
    CloseHandle(write_pipe);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, process.hProcess);
    }

    std::string pending;
    std::array<char, 4096> buffer{};
    const auto read_available_output = [&] {
        bool read_anything = false;
        while (true) {
            DWORD available = 0;
            PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr);
            if (available == 0) break;

            DWORD read = 0;
            ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
            if (read == 0) break;
            read_anything = true;
            pending.append(buffer.data(), read);
            result.output.append(buffer.data(), read);
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, newline);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (on_line) on_line(line);
                pending.erase(0, newline + 1);
            }
        }
        return read_anything;
    };

    while (true) {
        if (cancel_requested.exchange(false)) {
            TerminateProcess(process.hProcess, 1);
            result.cancelled = true;
        }

        read_available_output();

        if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    read_available_output();
    if (!pending.empty() && on_line) on_line(pending);

    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(read_pipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job != nullptr) CloseHandle(job);
    return result;
}
}
