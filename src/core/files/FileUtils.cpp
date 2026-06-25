#include "core/files/FileUtils.hpp"

#include <windows.h>

#include <cwctype>

namespace sto::files {
std::filesystem::path executable_directory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path unique_path(const std::filesystem::path& requested) {
    if (!std::filesystem::exists(requested)) return requested;
    for (int index = 1; index < 10000; ++index) {
        const auto candidate = requested.parent_path() / (requested.stem().wstring() + L"_" + std::to_wstring(index) + requested.extension().wstring());
        if (!std::filesystem::exists(candidate)) return candidate;
    }
    return requested;
}

std::wstring sanitize_stem(const std::wstring& stem) {
    std::wstring output;
    bool separator = false;
    for (wchar_t character : stem) {
        constexpr wchar_t accented[] = L"áàâãäÁÀÂÃÄéèêëÉÈÊËíìîïÍÌÎÏóòôõöÓÒÔÕÖúùûüÚÙÛÜçÇ";
        constexpr wchar_t ascii[] =    L"aaaaaAAAAAeeeeEEEEiiiiIIIIoooooOOOOOuuuuUUUUcC";
        if (const wchar_t* match = std::wcschr(accented, character)) {
            character = ascii[match - accented];
        }
        const bool accepted = std::iswalnum(character) || character == L'_';
        if (accepted) {
            output += character;
            separator = false;
        } else if (!separator && !output.empty()) {
            output += L'_';
            separator = true;
        }
    }
    while (!output.empty() && output.back() == L'_') output.pop_back();
    if (output.empty()) output = L"arquivo";
    if (output.size() > 180) output.resize(180);
    return output;
}

std::filesystem::path sanitized_output(const std::filesystem::path& input, const std::filesystem::path& directory, const std::wstring& extension) {
    return unique_path(directory / (sanitize_stem(input.stem().wstring()) + extension));
}

double size_megabytes(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0.0 : static_cast<double>(size) / (1024.0 * 1024.0);
}
}
