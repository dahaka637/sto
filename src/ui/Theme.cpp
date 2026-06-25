#include "ui/Theme.hpp"

#include "imgui.h"

#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace sto::ui {
namespace {
ImFont* g_brand_font   = nullptr;
ImFont* g_heading_font = nullptr;
ImFont* g_section_font = nullptr;

ImTextureID       g_doc_logo        = ImTextureID_Invalid;
int               g_doc_logo_width  = 0;
int               g_doc_logo_height = 0;
std::filesystem::path g_pending_doc_logo;

// Read a font file using Win32 wide-string APIs so that UNC network share paths
// and Unicode paths work reliably on all configurations — fopen() (used by ImGui
// internally) can silently fail on certain Windows 10 setups when the path is a
// network share, causing icons to fall back to "?".
// Returns ImGui-owned memory (allocated via ImGui::MemAlloc) that will be freed
// by the atlas after building, or nullptr on failure.
static void* load_font_win32(const std::filesystem::path& path, int* out_size) {
    *out_size = 0;
    HANDLE h = CreateFileW(
        path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(h, &file_size) ||
        file_size.QuadPart <= 0 || file_size.QuadPart > 32LL * 1024 * 1024) {
        CloseHandle(h);
        return nullptr;
    }
    const auto byte_count = static_cast<DWORD>(file_size.QuadPart);
    void* buf = ImGui::MemAlloc(byte_count);
    if (!buf) { CloseHandle(h); return nullptr; }
    DWORD read = 0;
    if (!ReadFile(h, buf, byte_count, &read, nullptr) || read != byte_count) {
        ImGui::MemFree(buf);
        CloseHandle(h);
        return nullptr;
    }
    CloseHandle(h);
    *out_size = static_cast<int>(byte_count);
    return buf;
}
}  // namespace

VisualSettings default_visual_settings() {
    ImGuiStyle base; // ImGui constructor sets its own defaults for all colors
    ImVec4* c = base.Colors;

    // STO overrides on top of ImGui defaults
    c[ImGuiCol_Text]                   = {0.93F, 0.94F, 0.95F, 1.00F};
    c[ImGuiCol_TextDisabled]           = {0.52F, 0.55F, 0.58F, 1.00F};
    c[ImGuiCol_WindowBg]               = {0.035F, 0.039F, 0.043F, 1.00F};
    c[ImGuiCol_ChildBg]                = {0.060F, 0.066F, 0.072F, 1.00F};
    c[ImGuiCol_PopupBg]                = {0.060F, 0.066F, 0.072F, 1.00F};
    c[ImGuiCol_ModalWindowDimBg]       = {0.015F, 0.018F, 0.020F, 0.72F};
    c[ImGuiCol_Border]                 = {0.20F, 0.22F, 0.24F, 1.00F};
    c[ImGuiCol_FrameBg]                = {0.095F, 0.105F, 0.115F, 1.00F};
    c[ImGuiCol_FrameBgHovered]         = {0.16F, 0.17F, 0.18F, 1.00F};
    c[ImGuiCol_FrameBgActive]          = {0.21F, 0.22F, 0.23F, 1.00F};
    c[ImGuiCol_Button]                 = {0.105F, 0.115F, 0.125F, 1.00F};
    c[ImGuiCol_ButtonHovered]          = {0.18F, 0.19F, 0.20F, 1.00F};
    c[ImGuiCol_ButtonActive]           = {0.26F, 0.27F, 0.28F, 1.00F};
    c[ImGuiCol_Header]                 = {0.16F, 0.17F, 0.18F, 1.00F};
    c[ImGuiCol_HeaderHovered]          = {0.22F, 0.23F, 0.24F, 1.00F};
    c[ImGuiCol_HeaderActive]           = {0.28F, 0.29F, 0.30F, 1.00F};
    c[ImGuiCol_Tab]                    = {0.085F, 0.095F, 0.105F, 1.00F};
    c[ImGuiCol_TabHovered]             = {0.20F, 0.21F, 0.22F, 1.00F};
    c[ImGuiCol_TabSelected]            = {0.27F, 0.28F, 0.29F, 1.00F};
    c[ImGuiCol_TabSelectedOverline]    = {0.78F, 0.80F, 0.82F, 1.00F};
    c[ImGuiCol_TabDimmed]              = {0.070F, 0.078F, 0.085F, 1.00F};
    c[ImGuiCol_TabDimmedSelected]      = {0.16F, 0.17F, 0.18F, 1.00F};
    c[ImGuiCol_Separator]              = {0.20F, 0.22F, 0.24F, 1.00F};
    c[ImGuiCol_CheckMark]              = {0.84F, 0.86F, 0.88F, 1.00F};

    VisualSettings s;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) s.colors[i] = base.Colors[i];
    return s;
}

void apply_visual_colors(const VisualSettings& settings) {
    ImVec4* colors = ImGui::GetStyle().Colors;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) colors[i] = settings.colors[i];
}

bool save_visual_settings(const VisualSettings& settings, const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    FILE* f = nullptr;
    _wfopen_s(&f, path.wstring().c_str(), L"w");
    if (!f) return false;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4& col = settings.colors[i];
        fprintf(f, "%d %.6f %.6f %.6f %.6f\n", i, col.x, col.y, col.z, col.w);
    }
    fclose(f);
    return true;
}

bool load_visual_settings(VisualSettings& settings, const std::filesystem::path& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.wstring().c_str(), L"r");
    if (!f) return false;
    int idx;
    float r, g, b, a;
    while (fscanf_s(f, "%d %f %f %f %f", &idx, &r, &g, &b, &a) == 5) {
        if (idx >= 0 && idx < ImGuiCol_COUNT)
            settings.colors[idx] = {r, g, b, a};
    }
    fclose(f);
    return true;
}

void initialize_fonts(ImGuiIO& io, const std::filesystem::path& asset_directory) {
    // System fonts are on C:\ (always local) — fopen() works fine.
    constexpr const char* regular_font = "C:\\Windows\\Fonts\\segoeui.ttf";
    constexpr const char* semibold_font = "C:\\Windows\\Fonts\\seguisb.ttf";

    if (ImFont* font = io.Fonts->AddFontFromFileTTF(regular_font, 18.0F)) {
        io.FontDefault = font;
    }

    // Icon font lives in the assets directory which may be on a UNC network share.
    // Load via CreateFileW, then hand the buffer to AddFontFromMemoryTTF which takes
    // ownership (FontDataOwnedByAtlas = true, the default) and frees it after building.
    int icon_size = 0;
    void* icon_data = load_font_win32(
        asset_directory / "fonts" / "Font Awesome 7 Free-Solid-900.otf", &icon_size);
    if (icon_data) {
        ImFontConfig icon_cfg;
        icon_cfg.MergeMode = true;
        icon_cfg.PixelSnapH = true;
        icon_cfg.GlyphMinAdvanceX = 17.0F;
        static constexpr ImWchar icon_ranges[] = {0xF000, 0xF8FF, 0};
        io.Fonts->AddFontFromMemoryTTF(icon_data, icon_size, 16.0F, &icon_cfg, icon_ranges);
    }

    g_brand_font   = io.Fonts->AddFontFromFileTTF(semibold_font, 36.0F);
    g_heading_font = io.Fonts->AddFontFromFileTTF(semibold_font, 28.0F);
    g_section_font = io.Fonts->AddFontFromFileTTF(semibold_font, 20.0F);
}

void apply_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding    = {18.0F, 18.0F};
    style.FramePadding     = {12.0F, 8.0F};
    style.ItemSpacing      = {12.0F, 10.0F};
    style.ItemInnerSpacing = {8.0F, 6.0F};
    style.ScrollbarSize    = 13.0F;
    style.WindowRounding   = 0.0F;
    style.ChildRounding    = 5.0F;
    style.FrameRounding    = 4.0F;
    style.PopupRounding    = 8.0F;
    style.ScrollbarRounding= 8.0F;
    style.GrabRounding     = 6.0F;

    apply_visual_colors(default_visual_settings());
}

ImFont* brand_font()   { return g_brand_font; }
ImFont* heading_font() { return g_heading_font; }
ImFont* section_font() { return g_section_font; }

void set_doc_logo(ImTextureID id, int w, int h) {
    g_doc_logo = id; g_doc_logo_width = w; g_doc_logo_height = h;
}
ImTextureID doc_logo_texture() { return g_doc_logo; }
int         doc_logo_width()   { return g_doc_logo_width; }
int         doc_logo_height()  { return g_doc_logo_height; }

void request_doc_logo_reload(const std::filesystem::path& path) { g_pending_doc_logo = path; }
std::filesystem::path pending_doc_logo_path() { return g_pending_doc_logo; }
void clear_pending_doc_logo() { g_pending_doc_logo.clear(); }

}
