#pragma once

#include <filesystem>
#include <string>

namespace sto::files {
std::filesystem::path executable_directory();
std::filesystem::path unique_path(const std::filesystem::path& requested);
std::wstring sanitize_stem(const std::wstring& stem);
std::filesystem::path sanitized_output(const std::filesystem::path& input, const std::filesystem::path& directory, const std::wstring& extension);
double size_megabytes(const std::filesystem::path& path);
}

