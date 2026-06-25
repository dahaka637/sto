#include "ui/MainMenu.hpp"

#include "core/RuntimeCheck.hpp"
#include "core/files/FileUtils.hpp"
#include "imgui.h"
#include "modules/audio/AudioModule.hpp"
#include "modules/documents/DocumentsModule.hpp"
#include "modules/prompts/PromptModule.hpp"
#include "modules/transcription/TranscriptionModule.hpp"
#include "modules/utilities/UtilitiesModule.hpp"
#include "ui/Theme.hpp"

#include <cfloat>
#include <filesystem>
#include <string>

namespace sto::ui {
namespace {
constexpr const char* kIconHome = "\xef\x80\x95";
constexpr const char* kIconMicrophone = "\xef\x84\xb0";
constexpr const char* kIconWand = "\xef\x83\x90";
constexpr const char* kIconShield = "\xef\x84\xb2";
constexpr const char* kIconTools = "\xef\x9f\x99";
constexpr const char* kIconDocuments = "\xef\x85\x9c";
constexpr const char* kIconGear = "\xef\x80\x93";
constexpr const char* kIconChevronLeft = "\xef\x81\x93";
constexpr const char* kIconChevronRight = "\xef\x81\x94";
constexpr const char* kIconInfo       = "\xef\x81\x9a";
constexpr const char* kIconHeadphones = "\xef\x80\xa5";
constexpr const char* kIconReset = "\xe2\x86\xba"; // U+21BA ↺

VisualSettings g_visual;
VisualSettings g_visual_defaults;
bool g_visual_initialized = false;
bool g_visual_reset_confirm = false;
std::filesystem::path g_visual_cfg_path;

enum class Page {
    home,
    transcription,
    audio,
    prompts,
    utilities,
    documents,
    settings,
};

constexpr ImVec4 kWhite = {0.94F, 0.95F, 0.96F, 1.00F};
constexpr ImVec4 kSilver = {0.70F, 0.73F, 0.76F, 1.00F};
constexpr ImVec4 kMuted = {0.52F, 0.55F, 0.58F, 1.00F};
constexpr ImVec4 kSubtle = {0.36F, 0.39F, 0.42F, 1.00F};

Page g_current_page = Page::home;
bool g_sidebar_collapsed = false;
ImTextureID g_brand_logo = ImTextureID_Invalid;
int g_brand_logo_width = 0;
int g_brand_logo_height = 0;

void push_font(ImFont* font) {
    if (font != nullptr) ImGui::PushFont(font);
}

void pop_font(ImFont* font) {
    if (font != nullptr) ImGui::PopFont();
}

void draw_accent_line(float width) {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(position, {position.x + width, position.y + 3.0F}, ImGui::GetColorU32(kSilver));
    ImGui::Dummy({width, 3.0F});
}

bool sidebar_item(const char* icon, const char* label, bool selected) {
    const std::string text = g_sidebar_collapsed ? std::string(icon) : std::string(icon) + "  " + label;
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4{0.22F, 0.23F, 0.24F, 1.00F} : ImVec4{0.060F, 0.066F, 0.072F, 1.00F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.17F, 0.18F, 0.19F, 1.00F});
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? kWhite : kSilver);
    const bool clicked = ImGui::Button(text.c_str(), {-1.0F, 42.0F});
    ImGui::PopStyleColor(3);
    if (g_sidebar_collapsed && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label);
    return clicked;
}

void collapse_button(float sidebar_width) {
    constexpr float button_size = 28.0F;
    ImGui::SetCursorPos({sidebar_width - button_size - 10.0F, 10.0F});
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.060F, 0.066F, 0.072F, 0.00F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.17F, 0.18F, 0.19F, 1.00F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.24F, 0.25F, 0.26F, 1.00F});
    ImGui::PushStyleColor(ImGuiCol_Text, kSilver);
    if (ImGui::Button(g_sidebar_collapsed ? kIconChevronRight : kIconChevronLeft, {button_size, button_size})) {
        g_sidebar_collapsed = !g_sidebar_collapsed;
    }
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(g_sidebar_collapsed ? "Expandir menu" : "Recolher menu");
    }
}

void render_sidebar() {
    constexpr ImGuiWindowFlags sidebar_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const float sidebar_width = g_sidebar_collapsed ? 75.0F : 255.0F;
    ImGui::BeginChild("sidebar", {sidebar_width, 0.0F}, ImGuiChildFlags_Borders, sidebar_flags);
    collapse_button(sidebar_width);
    ImGui::SetCursorPosY(g_sidebar_collapsed ? 54.0F : 10.0F);

    if (!g_sidebar_collapsed) {
        if (g_brand_logo != ImTextureID_Invalid && g_brand_logo_width > 0 && g_brand_logo_height > 0) {
            const float logo_slot_width = 150.0F;
            const float logo_slot_height = logo_slot_width * static_cast<float>(g_brand_logo_height) / static_cast<float>(g_brand_logo_width);
            const float logo_draw_width = 146.0F;
            const float logo_draw_height = logo_draw_width * static_cast<float>(g_brand_logo_height) / static_cast<float>(g_brand_logo_width);
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - logo_slot_width) * 0.5F);
            const ImVec2 slot_min = ImGui::GetCursorScreenPos();
            const ImVec2 slot_max{slot_min.x + logo_slot_width, slot_min.y + logo_slot_height};
            const ImVec2 draw_min{
                slot_min.x + (logo_slot_width - logo_draw_width) * 0.5F,
                slot_min.y + (logo_slot_height - logo_draw_height) * 0.5F
            };
            const ImVec2 draw_max{draw_min.x + logo_draw_width, draw_min.y + logo_draw_height};
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(slot_min, slot_max, true);
            draw->AddImage(ImTextureRef(g_brand_logo), draw_min, draw_max);
            draw->PopClipRect();
            ImGui::Dummy({logo_slot_width, logo_slot_height});
        } else {
            push_font(brand_font());
            ImGui::TextColored(kWhite, "STO");
            pop_font(brand_font());
        }
        draw_accent_line(50.0F);
        ImGui::Spacing();
        ImGui::TextColored(kSilver, "Sistema de Transcrição de Oitivas");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (sidebar_item(kIconHome, "INÍCIO", g_current_page == Page::home)) g_current_page = Page::home;
    if (sidebar_item(kIconMicrophone,  "TRANSCRIÇÃO DE OITIVAS",  g_current_page == Page::transcription)) g_current_page = Page::transcription;
    if (sidebar_item(kIconHeadphones, "TRANSCRIÇÃO DE ÁUDIOS",   g_current_page == Page::audio))         g_current_page = Page::audio;
    if (sidebar_item(kIconDocuments,  "GERAÇÃO DE DOCUMENTOS",   g_current_page == Page::documents))     g_current_page = Page::documents;
    if (sidebar_item(kIconWand, "PROMPTS PARA IA", g_current_page == Page::prompts)) g_current_page = Page::prompts;
    if (sidebar_item(kIconTools, "FERRAMENTAS DIVERSAS", g_current_page == Page::utilities)) g_current_page = Page::utilities;

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 146.0F);
    ImGui::Separator();
    if (sidebar_item(kIconGear, "CONFIGURAÇÕES", g_current_page == Page::settings)) g_current_page = Page::settings;
    if (!g_sidebar_collapsed) ImGui::TextColored(kSubtle, "  STO  |  v1.0");
    ImGui::EndChild();
}

void render_page_header(const char* eyebrow, const char* title, const char* subtitle) {
    ImGui::TextColored(kSilver, "%s", eyebrow);
    ImGui::Spacing();
    push_font(heading_font());
    ImGui::TextColored(kWhite, "%s", title);
    pop_font(heading_font());
    ImGui::TextColored(kMuted, "%s", subtitle);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

void render_home() {
    constexpr float margin = 10.0F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0F, 8.0F});
    ImGui::BeginChild(
        "home-frame",
        {ImGui::GetContentRegionAvail().x - margin, 0.0F},
        false,
        ImGuiWindowFlags_NoScrollbar
    );

    // Cabeçalho centralizado
    ImGui::TextColored(kSilver, "INÍCIO");
    ImGui::Spacing();
    push_font(heading_font());
    {
        constexpr const char* kTitle = "STO";
        const float tw = ImGui::CalcTextSize(kTitle).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - tw) * 0.5F));
        ImGui::TextColored(kWhite, "%s", kTitle);
    }
    pop_font(heading_font());
    {
        constexpr const char* kSub = "Sistema de Transcrição de Oitivas e Áudios";
        const float sw = ImGui::CalcTextSize(kSub).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - sw) * 0.5F));
        ImGui::TextColored(kMuted, "%s", kSub);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Apresentação — sem frame, conteúdo integrado ao layout
    ImGui::TextColored(kSilver, "%s  SOBRE O STO", kIconInfo);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "O STO é um sistema local de transcrição automática por IA, focado nas rotinas documentais "
        "da unidade policial. Reúne transcrição de oitivas, transcrição de arquivos de áudio com Auto próprio, "
        "gerenciamento de prompts para modelos de IA generativa, geração de documentos e ferramentas de produtividade — tudo "
        "executado localmente no dispositivo, sem dependência de serviços externos."
    );
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Módulos
    push_font(section_font());
    ImGui::TextColored(kWhite, "Módulos");
    pop_font(section_font());
    ImGui::Spacing();

    constexpr ImVec4 kBlue   = {0.00F, 0.48F, 1.00F, 1.00F};
    constexpr ImVec4 kPurple = {0.44F, 0.26F, 0.76F, 1.00F};
    constexpr ImVec4 kOrange = {0.82F, 0.44F, 0.12F, 1.00F};
    constexpr ImVec4 kGreen  = {0.20F, 1.00F, 0.36F, 1.00F};
    constexpr ImVec4 kTeal   = {0.10F, 0.65F, 0.62F, 1.00F};
    constexpr ImVec4 kAudio  = {0.24F, 0.72F, 0.46F, 1.00F};

    const auto active_card = [&](ImVec4 accent, const char* icon, const char* name, const char* desc) {
        ImGui::TableNextColumn();
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x + 50.0F, p.y + 3.0F}, ImGui::GetColorU32(accent));
            ImGui::Dummy({50.0F, 3.0F});
        }
        ImGui::Spacing();
        ImGui::TextColored(accent, "%s  %s", icon, name);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", desc);
        ImGui::Spacing();
        ImGui::TextColored(kGreen, "  DISPONÍVEL");
        ImGui::Spacing();
    };

    const auto upcoming_card = [&](const char* icon, const char* name, const char* desc) {
        ImGui::TableNextColumn();
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x + 38.0F, p.y + 3.0F}, ImGui::GetColorU32(kSubtle));
            ImGui::Dummy({38.0F, 3.0F});
        }
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "%s  %s", icon, name);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kSubtle);
        ImGui::TextWrapped("%s", desc);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "  EM BREVE");
        ImGui::Spacing();
    };

    if (ImGui::BeginTable("home-modules-table", 2,
        ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        active_card(kBlue, kIconMicrophone, "TRANSCRIÇÃO DE OITIVAS",
            "Transcrição automática por IA (Whisper). Suporta áudio e vídeo, fila de processamento, "
            "limpeza de eco e geração de prompt estruturado para revisão.");
        active_card(kAudio, kIconHeadphones, "TRANSCRIÇÃO DE ÁUDIOS",
            "Transcrição em lote de arquivos de áudio com fila dedicada, estimativa de tempo, seleção múltipla "
            "e geração de Auto de Transcrição de Áudios com textos configuráveis.");
        active_card(kTeal, kIconDocuments, "GERAÇÃO DE DOCUMENTOS",
            "Geração de Auto de Transcrição e Auto de Transcrição Geral com preenchimento guiado "
            "e exportação em PDF, com pré-preenchimento automático a partir da fila de transcrições.");
        active_card(kPurple, kIconWand, "PROMPTS PARA IA",
            "Biblioteca de prompts customizáveis para modelos de IA generativa, com compositor "
            "integrado para formatação e envio do texto ao modelo.");
        active_card(kOrange, kIconTools, "FERRAMENTAS DIVERSAS",
            "Conversão de áudio, compressão, separação e junção de PDFs, correção de codec de vídeo "
            "e padronização automática de nomenclatura de arquivos.");
        ImGui::EndTable();
    }
    ImGui::Spacing();

    // Rodapé
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(kSubtle, "  STO  v1.0");

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void render_visual_settings_section() {
    constexpr ImGuiColorEditFlags picker_flags =
        ImGuiColorEditFlags_NoInputs |
        ImGuiColorEditFlags_NoLabel |
        ImGuiColorEditFlags_AlphaPreviewHalf;

    constexpr ImGuiTableFlags tbl_flags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_NoSavedSettings;

    const auto row = [&](const char* label, int col_idx) {
        ImGui::PushID(col_idx);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "%s", label);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::ColorEdit4("##c", &g_visual.colors[col_idx].x, picker_flags)) {
            sto::ui::apply_visual_colors(g_visual);
            sto::ui::save_visual_settings(g_visual, g_visual_cfg_path);
        }
        ImGui::PopID();
    };

    const auto begin_group = [](const char* name) -> bool {
        return ImGui::CollapsingHeader(name);
    };

    const auto with_table = [&](const char* id, auto&& body) {
        if (ImGui::BeginTable(id, 2, tbl_flags)) {
            ImGui::TableSetupColumn("##n", ImGuiTableColumnFlags_WidthStretch, 5.0F);
            ImGui::TableSetupColumn("##p", ImGuiTableColumnFlags_WidthFixed, 36.0F);
            body();
            ImGui::EndTable();
        }
        ImGui::Spacing();
    };

    if (begin_group("Texto")) {
        with_table("##ct", [&] {
            row("Texto principal",         ImGuiCol_Text);
            row("Texto desabilitado",      ImGuiCol_TextDisabled);
            row("Link de texto",           ImGuiCol_TextLink);
            row("Texto selecionado",       ImGuiCol_TextSelectedBg);
        });
    }

    if (begin_group("Fundos")) {
        with_table("##cf", [&] {
            row("Janela principal",        ImGuiCol_WindowBg);
            row("Painel filho",            ImGuiCol_ChildBg);
            row("Popup / menu",            ImGuiCol_PopupBg);
            row("Barra de menu",           ImGuiCol_MenuBarBg);
        });
    }

    if (begin_group("Bordas e Separadores")) {
        with_table("##cb", [&] {
            row("Borda",                   ImGuiCol_Border);
            row("Sombra de borda",         ImGuiCol_BorderShadow);
            row("Separador",               ImGuiCol_Separator);
            row("Separador (hover)",       ImGuiCol_SeparatorHovered);
            row("Separador (ativo)",       ImGuiCol_SeparatorActive);
        });
    }

    if (begin_group("Campos de entrada")) {
        with_table("##cc", [&] {
            row("Fundo de campo",          ImGuiCol_FrameBg);
            row("Campo (hover)",           ImGuiCol_FrameBgHovered);
            row("Campo (ativo)",           ImGuiCol_FrameBgActive);
            row("Cursor de texto",         ImGuiCol_InputTextCursor);
        });
    }

    if (begin_group("Botões")) {
        with_table("##cbu", [&] {
            row("Botão",                   ImGuiCol_Button);
            row("Botão (hover)",           ImGuiCol_ButtonHovered);
            row("Botão (ativo)",           ImGuiCol_ButtonActive);
        });
    }

    if (begin_group("Cabeçalhos e Listas")) {
        with_table("##ch", [&] {
            row("Cabeçalho",               ImGuiCol_Header);
            row("Cabeçalho (hover)",       ImGuiCol_HeaderHovered);
            row("Cabeçalho (ativo)",       ImGuiCol_HeaderActive);
            row("Linhas de árvore",        ImGuiCol_TreeLines);
        });
    }

    if (begin_group("Barra de Rolagem")) {
        with_table("##cs", [&] {
            row("Fundo",                   ImGuiCol_ScrollbarBg);
            row("Alça",                    ImGuiCol_ScrollbarGrab);
            row("Alça (hover)",            ImGuiCol_ScrollbarGrabHovered);
            row("Alça (ativa)",            ImGuiCol_ScrollbarGrabActive);
        });
    }

    if (begin_group("Controles")) {
        with_table("##cctl", [&] {
            row("Marca de seleção",        ImGuiCol_CheckMark);
            row("Fundo de checkbox",       ImGuiCol_CheckboxSelectedBg);
            row("Alça de slider",          ImGuiCol_SliderGrab);
            row("Alça de slider (ativa)",  ImGuiCol_SliderGrabActive);
        });
    }

    if (begin_group("Abas")) {
        with_table("##cta", [&] {
            row("Aba",                           ImGuiCol_Tab);
            row("Aba (hover)",                   ImGuiCol_TabHovered);
            row("Aba selecionada",               ImGuiCol_TabSelected);
            row("Sublinhado de aba selecionada", ImGuiCol_TabSelectedOverline);
            row("Aba inativa",                   ImGuiCol_TabDimmed);
            row("Aba inativa selecionada",       ImGuiCol_TabDimmedSelected);
            row("Sublinhado de aba inativa",     ImGuiCol_TabDimmedSelectedOverline);
        });
    }

    if (begin_group("Barra de título")) {
        with_table("##ctb", [&] {
            row("Barra de título",          ImGuiCol_TitleBg);
            row("Barra de título (ativa)",  ImGuiCol_TitleBgActive);
            row("Barra de título (colaps)", ImGuiCol_TitleBgCollapsed);
        });
    }

    if (begin_group("Redimensionamento")) {
        with_table("##cr", [&] {
            row("Alça",                    ImGuiCol_ResizeGrip);
            row("Alça (hover)",            ImGuiCol_ResizeGripHovered);
            row("Alça (ativa)",            ImGuiCol_ResizeGripActive);
        });
    }

    if (begin_group("Tabelas")) {
        with_table("##ctbl", [&] {
            row("Cabeçalho",               ImGuiCol_TableHeaderBg);
            row("Borda forte",             ImGuiCol_TableBorderStrong);
            row("Borda leve",              ImGuiCol_TableBorderLight);
            row("Linha par",               ImGuiCol_TableRowBg);
            row("Linha ímpar",             ImGuiCol_TableRowBgAlt);
        });
    }

    if (begin_group("Sobreposições e Navegação")) {
        with_table("##cov", [&] {
            row("Escurecimento de modal",  ImGuiCol_ModalWindowDimBg);
            row("Cursor de navegação",     ImGuiCol_NavCursor);
            row("Destaque de janela",      ImGuiCol_NavWindowingHighlight);
            row("Escurecimento de janela", ImGuiCol_NavWindowingDimBg);
            row("Alvo de arrastar/soltar", ImGuiCol_DragDropTarget);
            row("Fundo de arrastar/soltar",ImGuiCol_DragDropTargetBg);
            row("Marcador não salvo",      ImGuiCol_UnsavedMarker);
        });
    }

    if (begin_group("Gráficos")) {
        with_table("##cgr", [&] {
            row("Linha",                   ImGuiCol_PlotLines);
            row("Linha (hover)",           ImGuiCol_PlotLinesHovered);
            row("Histograma",              ImGuiCol_PlotHistogram);
            row("Histograma (hover)",      ImGuiCol_PlotHistogramHovered);
        });
    }
}

void render_settings() {
    constexpr float margin = 10.0F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0F, 8.0F});
    ImGui::BeginChild("settings-frame", {ImGui::GetContentRegionAvail().x - margin, 0.0F}, false, ImGuiWindowFlags_None);

    ImGui::TextColored(kSilver, "SISTEMA  /  CONFIGURAÇÕES");
    ImGui::Spacing();
    push_font(heading_font());
    ImGui::TextColored(kWhite, "Configurações");
    pop_font(heading_font());
    ImGui::TextColored(kMuted, "Preferências gerais e configurações do aplicativo.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Seção VISUAL (colapsável) ----
    if (g_visual_reset_confirm) { ImGui::OpenPopup("##visual-reset"); g_visual_reset_confirm = false; }
    ImGui::SetNextWindowSize({360.0F, 0.0F}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##visual-reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kWhite, "Restaurar visual padrão?");
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "Todas as personalizações de cores serão perdidas.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Restaurar", {160.0F, 36.0F})) {
            g_visual = g_visual_defaults;
            sto::ui::apply_visual_colors(g_visual);
            sto::ui::save_visual_settings(g_visual, g_visual_cfg_path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", {140.0F, 36.0F})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    const bool visual_open = ImGui::CollapsingHeader("  VISUAL##vs_section");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Personalização de cores da interface");

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1.0F, 1.0F, 1.0F, 0.07F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{1.0F, 1.0F, 1.0F, 0.12F});
    ImGui::PushStyleColor(ImGuiCol_Text,          kSubtle);
    if (ImGui::SmallButton((std::string(kIconReset) + "  Restaurar visual padrão##reset_vis").c_str()))
        g_visual_reset_confirm = true;
    ImGui::PopStyleColor(4);

    if (visual_open) {
        ImGui::TextColored(kMuted, "Personalize as cores da interface. As alterações são aplicadas em tempo real.");
        ImGui::Spacing();
        render_visual_settings_section();
    }

    ImGui::Separator();
    ImGui::Spacing();

    // ---- Seção DOCUMENTO ----
    ImGui::TextColored(kSilver, "%s", kIconDocuments);
    ImGui::SameLine(0.0F, 6.0F);
    push_font(section_font());
    ImGui::TextColored(kSilver, "DOCUMENTO");
    pop_font(section_font());
    ImGui::TextColored(kMuted, "Cabeçalho, rodapé, CSS e logo dos documentos gerados.");
    ImGui::Spacing();
    sto::modules::documents::render_document_settings();

    ImGui::Separator();
    ImGui::Spacing();

    // ---- Seção PROMPTS DA TRANSCRIÇÃO ----
    ImGui::TextColored(kSilver, "%s", kIconGear);
    ImGui::SameLine(0.0F, 6.0F);
    push_font(section_font());
    ImGui::TextColored(kSilver, "PROMPTS DA TRANSCRIÇÃO");
    pop_font(section_font());
    ImGui::TextColored(kMuted, "Instruções de contextualização e procedimento enviadas ao Whisper.");
    ImGui::Spacing();
    sto::modules::transcription::render_prompt_settings();

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void render_content() {
    ImGui::BeginChild("content", {0.0F, 0.0F}, false, ImGuiWindowFlags_NoScrollbar);
    switch (g_current_page) {
    case Page::home: render_home(); break;
    case Page::transcription: sto::modules::transcription::render(); break;
    case Page::audio:         sto::modules::audio::render();         break;
    case Page::prompts: sto::modules::prompts::render(); break;
    case Page::utilities: sto::modules::utilities::render(); break;
    case Page::documents: sto::modules::documents::render(); break;
    case Page::settings: render_settings(); break;
    }
    ImGui::EndChild();
}
}

void set_brand_logo(ImTextureID texture, int width, int height) {
    g_brand_logo = texture;
    g_brand_logo_width = width;
    g_brand_logo_height = height;
}

void render_main_menu() {
    if (!g_visual_initialized) {
        g_visual_initialized = true;
        g_visual_defaults = sto::ui::default_visual_settings();
        g_visual = g_visual_defaults;
        g_visual_cfg_path = sto::files::executable_directory() / "data" / "visual.cfg";
        if (sto::ui::load_visual_settings(g_visual, g_visual_cfg_path))
            sto::ui::apply_visual_colors(g_visual);
    }

    static bool runtime_check_started = false;
    if (!runtime_check_started) {
        runtime_check_started = true;
        sto::runtime::start_check();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("STO", nullptr, flags);
    render_sidebar();
    ImGui::SameLine();
    render_content();
    sto::runtime::render_if_needed();
    sto::modules::transcription::render_notification();
    ImGui::End();
}
}
