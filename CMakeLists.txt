cmake_minimum_required(VERSION 3.28)

project(STO VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "STO currently supports Windows only.")
endif()

if(MSVC)
    # Statically link the C++ runtime so STO.exe does not depend on the system
    # MSVCP140.dll, which has a null-dereference bug at RVA 0x12C10 on certain
    # Windows 10 (19045) configurations.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(STO_RUNTIME_SOURCE
    ""
    CACHE PATH
    "Optional directory containing the FFmpeg and Ghostscript runtime folders"
)

set(STO_TRANSCRIPTION_SOURCE
    ""
    CACHE PATH
    "Optional directory containing the Whisper runtime, model and initial transcription prompts"
)

set(STO_PDF2DOCX_SOURCE
    ""
    CACHE PATH
    "Optional directory containing the pdf2docx runtime executable"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    fontawesome
    GIT_REPOSITORY https://github.com/FortAwesome/Font-Awesome.git
    GIT_TAG 7.2.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fontawesome)

FetchContent_Declare(
    sqlite
    URL https://sqlite.org/2026/sqlite-amalgamation-3530100.zip
)
FetchContent_MakeAvailable(sqlite)

FetchContent_Declare(
    pagedjs
    URL https://registry.npmjs.org/pagedjs/-/pagedjs-0.4.3.tgz
)
FetchContent_MakeAvailable(pagedjs)

add_library(sto_sqlite STATIC
    ${sqlite_SOURCE_DIR}/sqlite3.c
)

target_include_directories(sto_sqlite PUBLIC
    ${sqlite_SOURCE_DIR}
)

target_compile_definitions(sto_sqlite PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
)

add_library(sto_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)

target_include_directories(sto_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_compile_definitions(sto_imgui PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

add_executable(sto WIN32
    src/main.cpp
    src/resources/sto.rc
    src/core/CrashReporter.cpp
    src/core/CrashReporter.hpp
    src/core/database/AudioRepository.cpp
    src/core/database/AudioRepository.hpp
    src/core/database/DocumentsRepository.cpp
    src/core/database/DocumentsRepository.hpp
    src/core/documents/HtmlDocument.cpp
    src/core/documents/HtmlDocument.hpp
    src/core/database/PromptRepository.cpp
    src/core/database/PromptRepository.hpp
    src/core/database/TranscriptionRepository.cpp
    src/core/database/TranscriptionRepository.hpp
    src/core/platform/FileDialogs.cpp
    src/core/platform/FileDialogs.hpp
    src/core/platform/ProcessRunner.cpp
    src/core/platform/ProcessRunner.hpp
    src/core/files/FileUtils.cpp
    src/core/files/FileUtils.hpp
    src/core/RuntimeCheck.cpp
    src/core/RuntimeCheck.hpp
    src/core/text/PortugueseDate.hpp
    src/modules/audio/AudioModule.cpp
    src/modules/audio/AudioModule.hpp
    src/modules/documents/DocumentsModule.cpp
    src/modules/documents/DocumentsModule.hpp
    src/modules/prompts/PromptModule.cpp
    src/modules/prompts/PromptModule.hpp
    src/modules/transcription/TranscriptionModule.cpp
    src/modules/transcription/TranscriptionModule.hpp
    src/modules/utilities/UtilitiesModule.cpp
    src/modules/utilities/UtilitiesModule.hpp
    src/platform/windows/D3D11Application.cpp
    src/platform/windows/D3D11Application.hpp
    src/ui/MainMenu.cpp
    src/ui/MainMenu.hpp
    src/ui/TextWrap.hpp
    src/ui/Theme.cpp
    src/ui/Theme.hpp
)

target_link_libraries(sto PRIVATE
    sto_imgui
    sto_sqlite
    d3d11
    dxgi
    shcore
    comdlg32
    shell32
    ole32
    windowscodecs
    gdi32
    advapi32
    dbghelp
    psapi
)

target_include_directories(sto PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(sto PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(sto PRIVATE /W4 /utf-8 /permissive-)
    target_compile_options(sto_imgui PRIVATE /utf-8)
endif()

set_target_properties(sto PROPERTIES
    OUTPUT_NAME "STO"
)

include(CTest)

if(BUILD_TESTING)
    add_executable(prompt_repository_test
        tests/PromptRepositoryTest.cpp
        src/core/database/PromptRepository.cpp
        src/core/database/PromptRepository.hpp
    )
    target_include_directories(prompt_repository_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(prompt_repository_test PRIVATE sto_sqlite)
    add_test(NAME prompt_repository_test COMMAND prompt_repository_test)

    add_executable(process_runner_test
        tests/ProcessRunnerTest.cpp
        src/core/platform/ProcessRunner.cpp
        src/core/platform/ProcessRunner.hpp
    )
    target_include_directories(process_runner_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME process_runner_test COMMAND process_runner_test)

    add_executable(transcription_repository_test
        tests/TranscriptionRepositoryTest.cpp
        src/core/database/TranscriptionRepository.cpp
        src/core/database/TranscriptionRepository.hpp
    )
    target_include_directories(transcription_repository_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(transcription_repository_test PRIVATE sto_sqlite)
    add_test(NAME transcription_repository_test COMMAND transcription_repository_test)

    add_executable(documents_repository_test
        tests/DocumentsRepositoryTest.cpp
        src/core/database/DocumentsRepository.cpp
        src/core/database/DocumentsRepository.hpp
    )
    target_include_directories(documents_repository_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(documents_repository_test PRIVATE sto_sqlite)
    add_test(NAME documents_repository_test COMMAND documents_repository_test)

    add_executable(html_document_test
        tests/HtmlDocumentTest.cpp
        src/core/documents/HtmlDocument.cpp
        src/core/documents/HtmlDocument.hpp
    )
    target_include_directories(html_document_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(html_document_test PRIVATE shell32)
    add_test(NAME html_document_test COMMAND html_document_test)
endif()

add_custom_command(TARGET sto POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sto>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${fontawesome_SOURCE_DIR}/otfs/Font Awesome 7 Free-Solid-900.otf"
        "$<TARGET_FILE_DIR:sto>/assets/fonts/Font Awesome 7 Free-Solid-900.otf"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sto>/assets/branding"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/logo.png"
        "$<TARGET_FILE_DIR:sto>/assets/branding/logo.png"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sto>/assets/documents"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/documents/logo_pcsc.png"
        "$<TARGET_FILE_DIR:sto>/assets/documents/logo_pcsc.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${pagedjs_SOURCE_DIR}/dist/paged.polyfill.js"
        "$<TARGET_FILE_DIR:sto>/assets/documents/paged.polyfill.js"
)

if(STO_RUNTIME_SOURCE AND EXISTS "${STO_RUNTIME_SOURCE}/ffmpeg/bin/ffmpeg.exe")
    add_custom_command(TARGET sto POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sto>/runtime/ffmpeg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_RUNTIME_SOURCE}/ffmpeg/bin/ffmpeg.exe"
        "$<TARGET_FILE_DIR:sto>/runtime/ffmpeg/ffmpeg.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_RUNTIME_SOURCE}/ffmpeg/bin/ffprobe.exe"
        "$<TARGET_FILE_DIR:sto>/runtime/ffmpeg/ffprobe.exe"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sto>/runtime/ghostscript"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_RUNTIME_SOURCE}/ghostscript/bin/gswin64c.exe"
        "$<TARGET_FILE_DIR:sto>/runtime/ghostscript/gswin64c.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_RUNTIME_SOURCE}/ghostscript/bin/gsdll64.dll"
        "$<TARGET_FILE_DIR:sto>/runtime/ghostscript/gsdll64.dll"
    )
else()
    message(STATUS "STO runtime tools not configured; FFmpeg and Ghostscript will not be copied.")
endif()

if(STO_PDF2DOCX_SOURCE AND EXISTS "${STO_PDF2DOCX_SOURCE}/pdf2docx.exe")
    add_custom_command(TARGET sto POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sto>/runtime/pdf2docx"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${STO_PDF2DOCX_SOURCE}"
        "$<TARGET_FILE_DIR:sto>/runtime/pdf2docx"
    )
else()
    message(STATUS "STO PDF to Word runtime not configured; pdf2docx will not be copied.")
endif()

if(STO_TRANSCRIPTION_SOURCE AND EXISTS "${STO_TRANSCRIPTION_SOURCE}/whisper/bin/whisper-cli.exe")
    add_custom_command(TARGET sto POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sto>/assets/transcription-prompts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_TRANSCRIPTION_SOURCE}/prompts/prompt_contexto.json"
        "$<TARGET_FILE_DIR:sto>/assets/transcription-prompts/prompt_contexto.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_TRANSCRIPTION_SOURCE}/prompts/prompt_procedimento_tradicional.json"
        "$<TARGET_FILE_DIR:sto>/assets/transcription-prompts/prompt_procedimento_tradicional.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_TRANSCRIPTION_SOURCE}/prompts/prompt_procedimento_formal.json"
        "$<TARGET_FILE_DIR:sto>/assets/transcription-prompts/prompt_procedimento_formal.json"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sto>/runtime/whisper"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_TRANSCRIPTION_SOURCE}/whisper/bin/whisper-cli.exe"
        "$<TARGET_FILE_DIR:sto>/runtime/whisper/whisper-cli.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_TRANSCRIPTION_SOURCE}/whisper/bin/libgcc_s_seh-1.dll"
        "$<TARGET_FILE_DIR:sto>/runtime/whisper/libgcc_s_seh-1.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_TRANSCRIPTION_SOURCE}/whisper/bin/libwinpthread-1.dll"
        "$<TARGET_FILE_DIR:sto>/runtime/whisper/libwinpthread-1.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_TRANSCRIPTION_SOURCE}/whisper/bin/libstdc++-6.dll"
        "$<TARGET_FILE_DIR:sto>/runtime/whisper/libstdc++-6.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_TRANSCRIPTION_SOURCE}/whisper/bin/libgomp-1.dll"
        "$<TARGET_FILE_DIR:sto>/runtime/whisper/libgomp-1.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STO_TRANSCRIPTION_SOURCE}/whisper/ggml-large-v3-turbo.bin"
        "$<TARGET_FILE_DIR:sto>/runtime/whisper/ggml-large-v3-turbo.bin"
    )
else()
    message(STATUS "STO transcription runtime not configured; Whisper assets will not be copied.")
endif()
