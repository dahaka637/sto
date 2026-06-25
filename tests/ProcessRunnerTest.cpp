#include "core/platform/ProcessRunner.hpp"

#include <atomic>
#include <iostream>

int main() {
    std::atomic_bool cancel = false;
    const auto result = sto::platform::run_process({L"cmd.exe", L"/c", L"echo 17"}, cancel);
    if (result.exit_code != 0 || result.output.find("17") == std::string::npos) {
        std::cerr << "A saída curta do subprocesso não foi capturada.\n";
        return 1;
    }
    return 0;
}
