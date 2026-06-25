#include "core/platform/FileDialogs.hpp"

#include <algorithm>

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

namespace sto::platform {
std::vector<std::filesystem::path> select_files(const std::wstring& title, const std::vector<FileFilter>& filters) {
    std::wstring filter_text;
    for (const auto& filter : filters) {
        filter_text += filter.label;
        filter_text.push_back(L'\0');
        filter_text += filter.pattern;
        filter_text.push_back(L'\0');
    }
    filter_text.push_back(L'\0');

    std::vector<wchar_t> buffer(65536, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrTitle = title.c_str();
    dialog.lpstrFilter = filter_text.c_str();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return {};

    std::vector<std::filesystem::path> files;
    const std::filesystem::path first = buffer.data();
    const wchar_t* cursor = buffer.data() + std::wcslen(buffer.data()) + 1;
    if (*cursor == L'\0') {
        files.push_back(first);
        return files;
    }
    while (*cursor != L'\0') {
        files.push_back(first / cursor);
        cursor += std::wcslen(cursor) + 1;
    }
    return files;
}

std::filesystem::path select_save_file(const std::wstring& title, const std::vector<FileFilter>& filters, const std::wstring& default_name) {
    std::wstring filter_text;
    std::wstring default_extension;
    for (const auto& filter : filters) {
        filter_text += filter.label;
        filter_text.push_back(L'\0');
        filter_text += filter.pattern;
        filter_text.push_back(L'\0');
        if (default_extension.empty()) {
            const std::size_t dot = filter.pattern.find_last_of(L'.');
            if (dot != std::wstring::npos) default_extension = filter.pattern.substr(dot + 1);
        }
    }
    filter_text.push_back(L'\0');

    std::vector<wchar_t> buffer(32768, L'\0');
    const std::size_t copy_length = std::min(default_name.size(), buffer.size() - 1);
    std::copy_n(default_name.c_str(), copy_length, buffer.data());

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrTitle = title.c_str();
    dialog.lpstrFilter = filter_text.c_str();
    dialog.lpstrDefExt = default_extension.empty() ? nullptr : default_extension.c_str();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return {};

    return std::filesystem::path(buffer.data());
}

std::filesystem::path select_directory(const std::wstring& title) {
    const HRESULT initialization = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(initialization);

    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        if (uninitialize) CoUninitialize();
        return {};
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title.c_str());

    std::filesystem::path selected;
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                selected = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (uninitialize) CoUninitialize();
    return selected;
}
}
