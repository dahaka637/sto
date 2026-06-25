#include "core/CrashReporter.hpp"
#include "platform/windows/D3D11Application.hpp"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    sto::crash::install();

    D3D11Application application;
    if (!application.initialize(instance, show_command)) {
        MessageBoxW(
            nullptr,
            L"Não foi possível inicializar o STO com DirectX 11.",
            L"STO - Erro de inicialização",
            MB_OK | MB_ICONERROR
        );
        return 1;
    }

    return application.run();
}

