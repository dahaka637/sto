#include "modules/prompts/PromptModule.hpp"

#include "core/database/PromptRepository.hpp"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ui/Theme.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <ranges>
#include <string>
#include <vector>
#include <windows.h>

namespace sto::modules::prompts {
namespace {
constexpr const char* kIconCopy = "\xef\x83\x85";
constexpr const char* kIconPlus = "\xef\x81\xa7";
constexpr const char* kIconSave = "\xef\x83\x87";
constexpr const char* kIconTrash = "\xef\x87\xb8";
constexpr const char* kIconPen = "\xef\x8c\x84";
constexpr const char* kIconWand = "\xef\x83\x90";
constexpr const char* kIconFile = "\xef\x85\x9b";
constexpr const char* kIconShield = "\xef\x84\xb2";
constexpr const char* kIconBalance = "\xef\x89\x8e";
constexpr const char* kIconSearch = "\xef\x80\x82";
constexpr const char* kIconClipboard = "\xef\x8c\xa8";
constexpr const char* kIconBrain = "\xef\x97\x9c";
constexpr const char* kIconBolt = "\xef\x83\xa7";
constexpr const char* kIconInfo = "\xef\x81\x9a";
constexpr const char* kIconWarning = "\xef\x81\xb1";
constexpr const char* kIconCheck = "\xef\x80\x8c";
constexpr ImVec4 kWhite = {0.94F, 0.95F, 0.96F, 1.00F};
constexpr ImVec4 kSilver = {0.70F, 0.73F, 0.76F, 1.00F};
constexpr ImVec4 kMuted = {0.52F, 0.55F, 0.58F, 1.00F};
constexpr ImVec4 kSuccess = {0.20F, 1.00F, 0.36F, 1.00F};
constexpr ImVec4 kError = {1.00F, 0.22F, 0.22F, 1.00F};
constexpr ImVec4 kYellow = {1.00F, 0.76F, 0.03F, 1.00F};
constexpr ImVec4 kGreen = {0.16F, 0.65F, 0.27F, 1.00F};
constexpr ImVec4 kBlue = {0.00F, 0.48F, 1.00F, 1.00F};
constexpr ImVec4 kRed = {0.86F, 0.21F, 0.27F, 1.00F};
constexpr ImVec4 kSelected = {0.08F, 0.23F, 0.38F, 1.00F};
constexpr ImVec4 kSelectedHovered = {0.10F, 0.31F, 0.50F, 1.00F};

enum class NoteKind { success, warning, error };

std::filesystem::path database_path() {
    wchar_t executable_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable_path, MAX_PATH);
    return std::filesystem::path(executable_path).parent_path() / "data" / "sto.db";
}

bool colored_button(const std::string& text, const ImVec2& size, const ImVec4& color, const ImVec4& text_color = kWhite) {
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {std::min(color.x + 0.10F, 1.0F), std::min(color.y + 0.10F, 1.0F), std::min(color.z + 0.10F, 1.0F), 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {color.x * 0.78F, color.y * 0.78F, color.z * 0.78F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    const bool clicked = ImGui::Button(text.c_str(), size);
    ImGui::PopStyleColor(4);
    return clicked;
}

std::string label(const char* icon, const char* text) {
    return std::string(icon) + "  " + text;
}

std::string utf8_from_codepoint(unsigned int codepoint) {
    std::string output;
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return output;
}

const std::vector<std::string>& available_icons() {
    static std::vector<std::string> icons;
    if (!icons.empty()) return icons;
    ImFont* font = ImGui::GetFont();
    if (font != nullptr) {
        ImFontBaked* baked = font->GetFontBaked(ImGui::GetFontSize());
        for (unsigned int codepoint = 0xF000; codepoint <= 0xF8FF; ++codepoint) {
            if (baked != nullptr && baked->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) != nullptr) {
                icons.push_back(utf8_from_codepoint(codepoint));
            }
        }
    }
    if (icons.empty()) {
        icons = {kIconWand, kIconFile, kIconShield, kIconBalance, kIconSearch, kIconClipboard, kIconBrain, kIconBolt, kIconInfo, kIconWarning, kIconCheck};
    }
    return icons;
}

bool copy_to_clipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(size) * sizeof(wchar_t));
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }
    wchar_t* destination = static_cast<wchar_t*>(GlobalLock(memory));
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, destination, size);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

std::string color_to_hex(const std::array<float, 3>& color) {
    char result[8]{};
    std::snprintf(result, sizeof(result), "#%02X%02X%02X",
        static_cast<int>(color[0] * 255.0F),
        static_cast<int>(color[1] * 255.0F),
        static_cast<int>(color[2] * 255.0F));
    return result;
}

std::array<float, 3> hex_to_color(const std::string& color) {
    unsigned red = 90, green = 100, blue = 112;
    if (color.size() == 7) ::sscanf_s(color.c_str(), "#%02x%02x%02x", &red, &green, &blue);
    return {red / 255.0F, green / 255.0F, blue / 255.0F};
}

struct PromptModule {
    sto::database::PromptRepository repository;
    std::vector<sto::database::Prompt> prompts;
    std::string source_text;
    std::array<char, 256> editor_title{};
    std::string editor_content;
    std::array<float, 3> editor_color{1.00F, 1.00F, 1.00F};
    std::string editor_icon = kIconWand;
    std::string pending_icon = kIconWand;
    int selected_id = 0;
    int editor_id = 0;
    bool initialized = false;
    bool database_ready = false;
    bool delete_confirmation = false;
    bool icon_popup = false;
    std::string status;
    NoteKind status_kind = NoteKind::success;
    std::chrono::steady_clock::time_point status_started{};

    void initialize() {
        if (initialized) return;
        initialized = true;
        std::string error;
        database_ready = repository.initialize(database_path(), error);
        if (!database_ready) set_status("Erro ao inicializar SQLite: " + error, NoteKind::error);
        else reload();
    }

    void reload() {
        std::string error;
        prompts = repository.list(error);
        if (!error.empty()) set_status("Erro ao carregar prompts: " + error, NoteKind::error);
        if (selected_id != 0 && std::ranges::none_of(prompts, [this](const auto& prompt) { return prompt.id == selected_id; })) selected_id = 0;
    }

    const sto::database::Prompt* selected() const {
        const auto iterator = std::ranges::find_if(prompts, [this](const auto& prompt) { return prompt.id == selected_id; });
        return iterator == prompts.end() ? nullptr : &*iterator;
    }

    void set_status(std::string message, NoteKind kind = NoteKind::success) {
        status = std::move(message);
        status_kind = kind;
        status_started = std::chrono::steady_clock::now();
    }

    void clear_status() { status.clear(); }

    void clear_editor() {
        editor_id = 0;
        editor_title.fill('\0');
        editor_content.clear();
        editor_color = {1.00F, 1.00F, 1.00F};
        editor_icon = kIconWand;
        pending_icon = editor_icon;
    }

    void load_editor(const sto::database::Prompt& prompt) {
        editor_id = prompt.id;
        std::snprintf(editor_title.data(), editor_title.size(), "%s", prompt.title.c_str());
        editor_content = prompt.content;
        editor_color = hex_to_color(prompt.color);
        editor_icon = prompt.icon.empty() ? kIconWand : prompt.icon;
        pending_icon = editor_icon;
    }

    void save_editor() {
        if (editor_title[0] == '\0' || editor_content.empty()) {
            set_status("Informe o título e o conteúdo do prompt.", NoteKind::warning);
            return;
        }
        sto::database::Prompt prompt{editor_id, editor_title.data(), editor_content, color_to_hex(editor_color), editor_icon};
        std::string error;
        if (!repository.save(prompt, error)) {
            set_status("Não foi possível salvar: " + error, NoteKind::error);
            return;
        }
        reload();
        set_status(editor_id == 0 ? "Prompt criado com sucesso." : "Prompt atualizado com sucesso.");
        clear_editor();
    }

    void remove_editor() {
        if (editor_id == 0) return;
        std::string error;
        if (!repository.remove(editor_id, error)) {
            set_status("Não foi possível excluir: " + error, NoteKind::error);
            return;
        }
        if (selected_id == editor_id) selected_id = 0;
        clear_editor();
        reload();
        set_status("Prompt excluído com sucesso.");
    }
};

PromptModule& state() {
    static PromptModule module;
    module.initialize();
    return module;
}

void render_inline_notification(PromptModule& module) {
    if (module.status.empty()) return;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - module.status_started).count();
    if (elapsed > 4.0) return;
    const float alpha = static_cast<float>(std::clamp(1.0 - std::max(0.0, elapsed - 3.2) / 0.8, 0.0, 1.0));
    const ImVec2 text_size = ImGui::CalcTextSize(module.status.c_str());
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 window_position = viewport->WorkPos;
    const ImVec2 window_size = viewport->WorkSize;
    const ImVec4 accent = module.status_kind == NoteKind::error ? kError : module.status_kind == NoteKind::warning ? kYellow : kSuccess;
    const ImVec4 faded_accent = {accent.x, accent.y, accent.z, accent.w * alpha};
    const float width = std::min(window_size.x - 40.0F, text_size.x + 42.0F);
    constexpr float height = 46.0F;
    const ImVec2 position = {
        window_position.x + (window_size.x - width) * 0.5F,
        window_position.y + (window_size.y - height) * 0.5F
    };
    const ImVec2 end = {position.x + width, position.y + height};
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && mouse.x >= position.x && mouse.x <= end.x
        && mouse.y >= position.y && mouse.y <= end.y) {
        module.clear_status();
        return;
    }
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(position, end, ImGui::GetColorU32(ImVec4{0.04F, 0.05F, 0.06F, 0.88F * alpha}), 8.0F);
    draw->AddRect(position, end, ImGui::GetColorU32(faded_accent), 8.0F, 0, 1.2F);
    draw->AddText({position.x + (width - text_size.x) * 0.5F, position.y + (height - text_size.y) * 0.5F}, ImGui::GetColorU32(faded_accent), module.status.c_str());
}

void render_prompt_combo(PromptModule& module) {
    const auto* selected = module.selected();
    const std::string preview = selected != nullptr ? (selected->icon + "  " + selected->title) : "Selecione um prompt";
    if (selected != nullptr) {
        const auto color = hex_to_color(selected->color);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{color[0], color[1], color[2], 1.00F});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0.095F, 0.105F, 0.115F, 1.00F});
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{0.16F, 0.17F, 0.18F, 1.00F});
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kSelected);
    }
    ImGui::SetNextItemWidth(-1.0F);
    const bool combo_open = ImGui::BeginCombo("##prompt-selector", preview.c_str());
    if (selected != nullptr) ImGui::PopStyleColor(4);
    if (!combo_open) return;

    if (module.prompts.empty()) ImGui::TextColored(kMuted, "Nenhum prompt cadastrado.");
    for (const auto& prompt : module.prompts) {
        const bool is_selected = module.selected_id == prompt.id;
        const auto color = hex_to_color(prompt.color);
        ImGui::PushStyleColor(ImGuiCol_Header, is_selected
            ? kSelected
            : ImVec4{0.095F, 0.105F, 0.115F, 1.00F});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, is_selected
            ? kSelectedHovered
            : ImVec4{0.16F, 0.17F, 0.18F, 1.00F});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, kSelected);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{color[0], color[1], color[2], 1.00F});
        const std::string item = (prompt.icon.empty() ? std::string(kIconWand) : prompt.icon) + "  " + prompt.title;
        if (ImGui::Selectable(item.c_str(), is_selected, 0, {0.0F, 34.0F})) module.selected_id = prompt.id;
        ImGui::PopStyleColor(4);
        if (is_selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}

void render_composer(PromptModule& module) {
    ImGui::BeginChild("composer", {0.0F, 0.0F}, ImGuiChildFlags_Borders);
    ImGui::TextColored(kSilver, "%s  COMPOSITOR", kIconWand);
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Prompt base");
    render_prompt_combo(module);
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Texto adicional para enviar à IA");
    ImGui::InputTextMultiline(
        "##source-text", &module.source_text, {-1.0F, -106.0F},
        ImGuiInputTextFlags_WordWrap
    );
    ImGui::Spacing();
    const float button_spacing = ImGui::GetStyle().ItemSpacing.x;
    const float button_width = (ImGui::GetContentRegionAvail().x - button_spacing) * 0.5F;
    if (colored_button(label(kIconTrash, "Limpar texto"), {button_width, 38.0F}, kRed)) {
        module.source_text.clear();
        module.set_status("Texto limpo com sucesso.");
    }
    ImGui::SameLine();
    if (colored_button(label(kIconCopy, "Copiar prompt completo"), {button_width, 38.0F}, kGreen)) {
        if (const auto* selected = module.selected()) {
            std::string composed = selected->content;
            if (!module.source_text.empty()) composed += "\n\nTexto para análise:\n" + module.source_text;
            const bool copied = copy_to_clipboard(composed);
            module.set_status(
                copied ? "Conteúdo copiado para a área de transferência." : "Não foi possível acessar a área de transferência.",
                copied ? NoteKind::success : NoteKind::error
            );
        } else module.set_status("Selecione um prompt antes de copiar.", NoteKind::warning);
    }
    ImGui::EndChild();
}

void render_library(PromptModule& module) {
    ImGui::BeginChild("library-list", {310.0F, 0.0F}, ImGuiChildFlags_Borders);
    ImGui::TextColored(kSilver, "BIBLIOTECA");
    ImGui::Spacing();
    if (ImGui::Button("Novo prompt", {-1.0F, 38.0F})) module.clear_editor();
    ImGui::Spacing();
    for (const auto& prompt : module.prompts) {
        const bool is_selected = module.editor_id == prompt.id;
        const auto color = hex_to_color(prompt.color);
        ImGui::PushStyleColor(ImGuiCol_Header, is_selected
            ? kSelected
            : ImVec4{0.095F, 0.105F, 0.115F, 1.00F});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, is_selected
            ? kSelectedHovered
            : ImVec4{0.16F, 0.17F, 0.18F, 1.00F});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, kSelected);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{color[0], color[1], color[2], 1.00F});
        const std::string item = (prompt.icon.empty() ? std::string(kIconWand) : prompt.icon) + "  " + prompt.title;
        if (ImGui::Selectable(item.c_str(), is_selected, 0, {0.0F, 34.0F})) module.load_editor(prompt);
        ImGui::PopStyleColor(4);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("library-editor", {0.0F, 0.0F}, ImGuiChildFlags_Borders);
    ImGui::TextColored(kSilver, "%s  %s", kIconPen, module.editor_id == 0 ? "NOVO PROMPT" : "EDITAR PROMPT");
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Título");
    ImGui::InputText("##editor-title", module.editor_title.data(), module.editor_title.size());
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Cor de identificação");
    ImGui::ColorEdit3("##editor-color", module.editor_color.data(), ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    const ImVec4 icon_color{module.editor_color[0], module.editor_color[1], module.editor_color[2], 1.0F};
    ImGui::PushStyleColor(ImGuiCol_Text, icon_color);
    if (ImGui::Button((module.editor_icon + "  Escolher ícone").c_str(), {170.0F, 0.0F})) {
        module.pending_icon = module.editor_icon;
        module.icon_popup = true;
    }
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Conteúdo do prompt");
    ImGui::InputTextMultiline(
        "##editor-content", &module.editor_content, {-1.0F, -104.0F},
        ImGuiInputTextFlags_WordWrap
    );
    ImGui::Spacing();
    const float action_spacing = ImGui::GetStyle().ItemSpacing.x;
    const float full_width = ImGui::GetContentRegionAvail().x;
    if (module.editor_id != 0) {
        const float action_width = (full_width - action_spacing) * 0.5F;
        if (colored_button(label(kIconSave, "Salvar prompt"), {action_width, 38.0F}, kGreen)) module.save_editor();
        ImGui::SameLine();
        if (colored_button(label(kIconTrash, "Excluir"), {action_width, 38.0F}, kRed)) module.delete_confirmation = true;
    } else {
        if (colored_button(label(kIconSave, "Salvar prompt"), {full_width, 38.0F}, kGreen)) module.save_editor();
    }
    ImGui::EndChild();

    if (module.icon_popup) ImGui::OpenPopup("Escolher ícone do prompt");
    ImGui::SetNextWindowSize({560.0F, 520.0F}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Escolher ícone do prompt", &module.icon_popup)) {
        ImGui::TextColored(kSilver, "Selecione um ícone para identificar este prompt");
        ImGui::Spacing();
        const ImVec4 color{module.editor_color[0], module.editor_color[1], module.editor_color[2], 1.0F};
        const float footer_height = ImGui::GetFrameHeight() + (ImGui::GetStyle().ItemSpacing.y * 2.0F);
        ImGui::BeginChild("icon-grid", {0.0F, std::max(220.0F, ImGui::GetContentRegionAvail().y - footer_height)}, ImGuiChildFlags_Borders);
        const float cell_size = 42.0F;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (cell_size + spacing)));
        const auto& icons = available_icons();
        for (int index = 0; index < static_cast<int>(icons.size()); ++index) {
            const std::string& icon = icons[static_cast<std::size_t>(index)];
            const bool selected_icon = module.pending_icon == icon;
            ImGui::PushID(index);
            ImGui::PushStyleColor(ImGuiCol_Button, selected_icon ? ImVec4{0.10F, 0.31F, 0.50F, 1.00F} : ImVec4{0.095F, 0.105F, 0.115F, 1.00F});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected_icon ? ImVec4{0.13F, 0.40F, 0.62F, 1.00F} : ImVec4{0.16F, 0.17F, 0.18F, 1.00F});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.08F, 0.23F, 0.38F, 1.00F});
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (ImGui::Button(icon.c_str(), {cell_size, cell_size})) {
                module.pending_icon = icon;
            }
            ImGui::PopStyleColor(4);
            if ((index + 1) % columns != 0) ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::Spacing();
        const float footer_spacing = ImGui::GetStyle().ItemSpacing.x;
        const float button_width = (ImGui::GetContentRegionAvail().x - footer_spacing) * 0.5F;
        if (colored_button(label(kIconCheck, "Selecionar"), {button_width, 36.0F}, kGreen)) {
            module.editor_icon = module.pending_icon;
            module.icon_popup = false;
        }
        ImGui::SameLine();
        if (colored_button("Fechar", {button_width, 36.0F}, kRed)) module.icon_popup = false;
        ImGui::EndPopup();
    }

    if (module.delete_confirmation) ImGui::OpenPopup("Confirmar exclusão");
    if (ImGui::BeginPopupModal("Confirmar exclusão", &module.delete_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Excluir este prompt permanentemente?");
        ImGui::Spacing();
        if (ImGui::Button("Cancelar", {110.0F, 36.0F})) module.delete_confirmation = false;
        ImGui::SameLine();
        if (colored_button("Excluir", {110.0F, 36.0F}, kRed)) {
            module.remove_editor();
            module.delete_confirmation = false;
        }
        ImGui::EndPopup();
    }
}
}

void render() {
    PromptModule& module = state();
    constexpr float horizontal_margin = 10.0F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + horizontal_margin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0F, 8.0F});
    ImGui::BeginChild(
        "prompts-module-frame",
        {ImGui::GetContentRegionAvail().x - horizontal_margin, 0.0F},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    ImGui::TextColored(kSilver, "MÓDULOS  /  PROMPTS PARA IA");
    ImGui::Spacing();
    if (sto::ui::heading_font() != nullptr) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "Prompts para IA");
    if (sto::ui::heading_font() != nullptr) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Separator();
    if (module.database_ready) {
        if (ImGui::BeginTabBar("prompt-tabs")) {
            if (ImGui::BeginTabItem("Compor e copiar")) {
                render_composer(module);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gerenciar biblioteca")) {
                render_library(module);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        render_inline_notification(module);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}
}
