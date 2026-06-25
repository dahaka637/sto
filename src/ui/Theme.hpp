#pragma once

#include "imgui.h"

#include <filesystem>

struct ImFont;
struct ImGuiIO;

namespace sto::ui {

struct VisualSettings {
    ImVec4 colors[ImGuiCol_COUNT]{};
};

void initialize_fonts(ImGuiIO& io, const std::filesystem::path& asset_directory);
void apply_theme();
void apply_visual_colors(const VisualSettings& settings);
VisualSettings default_visual_settings();
bool save_visual_settings(const VisualSettings& settings, const std::filesystem::path& path);
bool load_visual_settings(VisualSettings& settings, const std::filesystem::path& path);
ImFont* brand_font();
ImFont* heading_font();
ImFont* section_font();

// Document logo — loaded from a file by the platform layer on request
void set_doc_logo(ImTextureID id, int width, int height);
ImTextureID doc_logo_texture();
int doc_logo_width();
int doc_logo_height();
void request_doc_logo_reload(const std::filesystem::path& path);
std::filesystem::path pending_doc_logo_path();
void clear_pending_doc_logo();

}
