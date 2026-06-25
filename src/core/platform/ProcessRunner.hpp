#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace sto::platform {
struct ProcessResult {
    int exit_code = -1;
    std::string output;
    bool cancelled = false;
    bool started = false;  // false if CreateProcessW itself failed (exe not found, access denied)
};

ProcessResult run_process(
    const std::vector<std::wstring>& arguments,
    std::atomic_bool& cancel_requested,
    const std::function<void(const std::string&)>& on_line = {},
    const std::wstring& working_directory = {}
);
}

