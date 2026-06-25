#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace sto::platform {
struct FileFilter {
    std::wstring label;
    std::wstring pattern;
};

std::vector<std::filesystem::path> select_files(
    const std::wstring& title,
    const std::vector<FileFilter>& filters
);
std::filesystem::path select_directory(const std::wstring& title);
std::filesystem::path select_save_file(
    const std::wstring& title,
    const std::vector<FileFilter>& filters,
    const std::wstring& default_name
);
}

