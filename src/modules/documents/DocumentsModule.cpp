#include "modules/documents/DocumentsModule.hpp"

#include "core/database/DocumentsRepository.hpp"
#include "modules/transcription/TranscriptionModule.hpp"
#include "core/documents/HtmlDocument.hpp"
#include "core/files/FileUtils.hpp"
#include "core/platform/FileDialogs.hpp"
#include "core/text/PortugueseDate.hpp"
#include "imgui.h"
#include "ui/Theme.hpp"

// Markers replaced at document generation time:
//   $$LOGO_BASE64$$    – base64-encoded PNG of the institution logo
//   $$CONTENT$$        – body HTML generated from form data
//   $$BROWSER_TITLE$$  – document title used as PDF filename suggestion

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <filesystem>
#include <ranges>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace sto::modules::documents {
namespace {
namespace document_html = sto::documents;

constexpr const char* kIconBack = "\xef\x81\xa0";
constexpr const char* kIconPdf = "\xef\x87\x81";
constexpr const char* kIconCalendar = "\xef\x84\xb3";
constexpr const char* kIconPlus = "\xef\x81\xa7";
constexpr const char* kIconTrash = "\xef\x87\xb8";
constexpr const char* kIconChevronLeft = "\xef\x81\x93";
constexpr const char* kIconChevronRight = "\xef\x81\x94";
constexpr const char* kIconSave = "\xef\x83\x87";
constexpr const char* kIconFolder = "\xef\x81\xbb";
constexpr ImVec4 kWhite  = {0.94F, 0.95F, 0.96F, 1.00F};
constexpr ImVec4 kSilver = {0.70F, 0.73F, 0.76F, 1.00F};
constexpr ImVec4 kMuted  = {0.52F, 0.55F, 0.58F, 1.00F};
constexpr ImVec4 kSuccess= {0.20F, 1.00F, 0.36F, 1.00F};
constexpr ImVec4 kError  = {1.00F, 0.22F, 0.22F, 1.00F};
constexpr ImVec4 kBlue   = {0.00F, 0.48F, 1.00F, 1.00F};
constexpr ImVec4 kGreen  = {0.16F, 0.65F, 0.27F, 1.00F};
constexpr ImVec4 kRed    = {0.86F, 0.21F, 0.27F, 1.00F};
constexpr ImVec4 kSubtle = {0.36F, 0.39F, 0.42F, 1.00F};

std::string label(const char* icon, const char* text) {
    return std::string(icon) + "  " + text;
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

// Wraps long informational text at the current window's right edge instead
// of letting it overflow or get clipped when the window is narrowed.
void wrapped_text(const ImVec4& color, const std::string& text) {
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextColored(color, "%s", text.c_str());
    ImGui::PopTextWrapPos();
}

void accent_line(float width, const ImVec4& color = kSilver) {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(position, {position.x + width, position.y + 3.0F}, ImGui::GetColorU32(color), 2.0F);
    ImGui::Dummy({width, 3.0F});
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, output.data(), size);
    output.resize(static_cast<std::size_t>(size - 1));
    return output;
}

std::string capitalize(std::string text) {
    if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

constexpr std::array<const char*, 4> kTiposOitiva = {"Declaração", "Depoimento", "Interrogatório", "Depoimento Especial"};
constexpr std::array<const char*, 4> kTiposOitivaTexto = {"declaração", "depoimento", "interrogatório", "depoimento especial"};
constexpr std::array<const char*, 5> kTiposProcedimento = {
    "Inquérito Policial", "Auto de Prisão em Flagrante", "Termo Circunstanciado", "Verificação Preliminar de Informações", "Carta Precatória"
};
constexpr int kDepoimentoEspecialIndex = 3;
constexpr int kCartaPrecatoriaIndex = 4;

std::string oitiva_lowercase(int index) {
    return kTiposOitivaTexto[static_cast<std::size_t>(std::clamp(index, 0, 3))];
}

std::string procedimento_label(int index) {
    return kTiposProcedimento[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(kTiposProcedimento.size() - 1)))];
}

std::string artigo_procedimento(int index) {
    // "da" for feminine nouns: Carta Precatória, Verificação Preliminar de Informações
    return (index == kCartaPrecatoriaIndex || index == 3) ? "da " : "do ";
}

std::string procedure_sigla(int index) {
    switch (index) {
    case 0: return "IP";
    case 1: return "APF";
    case 2: return "TC";
    case 3: return "VPI";
    case 4: return "CP";
    default: return "";
    }
}

std::string qualificacao_padrao(int tipo_oitiva) {
    switch (std::clamp(tipo_oitiva, 0, 3)) {
    case 0: return "declarante";
    case 1: return "testemunha";
    case 2: return "interrogado";
    case 3: return "vítima, em depoimento especial";
    default: return "declarante";
    }
}

std::string roman_numeral(std::size_t value) {
    if (value == 0 || value > 3999) return std::to_string(value);
    struct RomanPart { std::size_t value; const char* numeral; };
    static constexpr std::array parts = {
        RomanPart{1000, "M"}, RomanPart{900, "CM"}, RomanPart{500, "D"}, RomanPart{400, "CD"},
        RomanPart{100, "C"}, RomanPart{90, "XC"}, RomanPart{50, "L"}, RomanPart{40, "XL"},
        RomanPart{10, "X"}, RomanPart{9, "IX"}, RomanPart{5, "V"}, RomanPart{4, "IV"},
        RomanPart{1, "I"},
    };

    std::string output;
    for (const auto& part : parts) {
        while (value >= part.value) {
            output += part.numeral;
            value -= part.value;
        }
    }
    return output;
}

struct FormData {
    std::string nome_delegado;
    int tipo_oitiva = 0;
    std::string nome_parte;
    std::string data_oitiva;
    int tipo_procedimento = 0;
    std::string numero_procedimento;
    std::string local_depoimento_especial;
    std::string relato;
    std::string nome_responsavel;
    std::string cargo_responsavel;
};

struct GeneralTranscriptionEntry {
    int tipo_oitiva = 0;
    std::string nome_parte;
    std::string qualificacao;
    std::string data_oitiva;
    std::string relato;
    std::size_t insertion_order = 0;
};

struct GeneralFormData {
    std::string nome_delegado;
    int tipo_procedimento = 0;
    std::string numero_procedimento;
    std::vector<GeneralTranscriptionEntry> oitivas;
    std::string nome_responsavel;
    std::string cargo_responsavel;
};

std::string auto_transcricao_style() {
    return R"HTML(
    .relato-box { margin: 1mm 0; border: 1px solid #000; padding: 1mm 1.2mm; font-size: 11pt; line-height: 14pt; }
    .relato-box p { text-align: justify; margin-bottom: 2mm; }
    .relato-box p:last-child { margin-bottom: 0; }

    .closing { margin-top: 2mm; text-align: justify; text-indent: 15mm; }

    .signature { margin-top: 12mm; text-align: center; font-size: 11pt; line-height: 12pt; }
    .signature .name { font-weight: 700; }
)HTML";
}

std::string replace_marker(std::string html, const std::string& marker, const std::string& value) {
    std::size_t pos = 0;
    while ((pos = html.find(marker, pos)) != std::string::npos) {
        html.replace(pos, marker.size(), value);
        pos += value.size();
    }
    return html;
}

std::string header_text_to_html(const std::string& text) {
    std::string result;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty()) {
            result += "<span class=\"line\">";
            result += document_html::escape_html(line);
            result += "</span>\n";
        }
    }
    return result;
}

std::string footer_text_to_html(const std::string& text) {
    // Footer is stored as raw HTML — the user can use <b>, <p>, etc.
    return text;
}

std::string build_opening_paragraph(const FormData& form, const std::string& data_extenso, const std::string& intro_template) {
    const std::string procedimento_str = artigo_procedimento(form.tipo_procedimento)
        + procedimento_label(form.tipo_procedimento) + " nº " + form.numero_procedimento;

    if (!intro_template.empty()) {
        std::string text = intro_template;
        text = replace_marker(text, "$$DATA_EXTENSO$$",  data_extenso);
        text = replace_marker(text, "$$NOME_DELEGADO$$", form.nome_delegado);
        text = replace_marker(text, "$$TIPO_OITIVA$$",   oitiva_lowercase(form.tipo_oitiva));
        text = replace_marker(text, "$$NOME_PARTE$$",    form.nome_parte);
        text = replace_marker(text, "$$DATA_OITIVA$$",   form.data_oitiva);
        text = replace_marker(text, "$$PROCEDIMENTO$$",  procedimento_str);
        text = replace_marker(text, "$$LOCAL_ESPECIAL$$",form.local_depoimento_especial);
        return "  <p class=\"justified\">" + document_html::escape_html(text) + "</p>\n\n";
    }

    // Hardcoded fallback
    std::string html = "  <p class=\"justified\"><b>Aos ";
    html += document_html::escape_html(data_extenso);
    html += "</b>, por determinação do Excelentíssimo Senhor Delegado de Polícia Civil, ";
    html += document_html::escape_html(form.nome_delegado);
    html += ", procedi à transcrição, DE FORMA RESUMIDA E NÃO NECESSARIAMENTE COM AS MESMAS PALAVRAS, do teor ";
    if (form.tipo_oitiva == kDepoimentoEspecialIndex) {
        html += "do ";
        html += document_html::escape_html(oitiva_lowercase(form.tipo_oitiva));
        html += " de <b><u>";
        html += document_html::escape_html(form.nome_parte);
        html += "</u></b>, colhido no dia ";
        html += document_html::escape_html(form.data_oitiva);
        html += ", ";
        html += document_html::escape_html(form.local_depoimento_especial);
        html += ", nos autos ";
        html += document_html::escape_html(procedimento_str);
    } else {
        html += "do(a) ";
        html += document_html::escape_html(oitiva_lowercase(form.tipo_oitiva));
        html += " de <b><u>";
        html += document_html::escape_html(form.nome_parte);
        html += "</u></b>, colhido(a) no dia ";
        html += document_html::escape_html(form.data_oitiva);
        html += ", no âmbito ";
        html += document_html::escape_html(procedimento_str);
    }
    html += ", conforme abaixo:</p>\n\n";
    return html;
}

std::string build_auto_transcricao_body(const FormData& form,
    const std::string& intro_template, const std::string& closing_text) {
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    const std::chrono::year_month_day today_ymd{today};
    const std::string data_extenso = sto::text::format_data_extenso(
        static_cast<int>(static_cast<unsigned>(today_ymd.day())),
        static_cast<unsigned>(today_ymd.month()),
        static_cast<int>(today_ymd.year())
    );

    std::string html = build_opening_paragraph(form, data_extenso, intro_template);
    html += "  <div class=\"relato-box\">\n";
    html += document_html::paragraphs_from_wrapped_text(form.relato);
    html += "  </div>\n\n";
    html += "  <p class=\"closing\">";
    html += document_html::escape_html(closing_text.empty()
        ? "Nada mais havendo a constar, procedo ao encerramento da presente transcrição, vai devidamente assinado, na forma da Lei."
        : closing_text);
    html += "</p>\n\n";
    html += "  <div class=\"signature\">\n    <div class=\"name\">";
    html += document_html::escape_html(form.nome_responsavel);
    html += "</div>\n    <div>";
    html += document_html::escape_html(form.cargo_responsavel);
    html += "</div>\n  </div>\n";
    return html;
}

std::vector<std::size_t> sorted_general_entry_indices(const std::vector<GeneralTranscriptionEntry>& entries) {
    std::vector<std::size_t> indices(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) indices[index] = index;

    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t left, std::size_t right) {
        const auto left_date = sto::text::parse_date_ddmmyyyy(entries[left].data_oitiva);
        const auto right_date = sto::text::parse_date_ddmmyyyy(entries[right].data_oitiva);
        if (left_date && right_date) {
            const auto left_days = std::chrono::sys_days{*left_date};
            const auto right_days = std::chrono::sys_days{*right_date};
            if (left_days != right_days) return left_days < right_days;
        } else if (left_date || right_date) {
            return left_date.has_value();
        }
        return entries[left].insertion_order < entries[right].insertion_order;
    });

    return indices;
}

std::string auto_transcricao_geral_style() {
    return R"HTML(
    .oitiva { margin-top: 4.5mm; }
    .content > p + .oitiva { margin-top: 5.6mm; }
    .oitiva-title { font-size: 11pt; line-height: 12pt; font-weight: 700; break-after: avoid; page-break-after: avoid; }
    .oitiva-title .texto { text-decoration: underline; }

    .transcricao-box { margin-top: 2mm; border: 1px solid #000; padding: 1mm 1.2mm; font-size: 11pt; line-height: 14pt; }
    .transcricao-box p { text-align: justify; margin-bottom: 2mm; }
    .transcricao-box p:last-child { margin-bottom: 0; }

    .closing { margin-top: 7.5mm; text-align: justify; text-indent: 15mm; }

    .signature { margin-top: 12mm; text-align: center; font-size: 11pt; line-height: 12pt; }
    .signature .name { font-weight: 700; }
)HTML";
}

std::string build_auto_transcricao_geral_body(const GeneralFormData& form,
    const std::string& intro_template, const std::string& closing_text,
    const std::string& oitiva_template) {
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    const std::chrono::year_month_day today_ymd{today};
    const std::string data_extenso = sto::text::format_data_extenso(
        static_cast<int>(static_cast<unsigned>(today_ymd.day())),
        static_cast<unsigned>(today_ymd.month()),
        static_cast<int>(today_ymd.year())
    );
    const std::string procedimento_str = artigo_procedimento(form.tipo_procedimento)
        + procedimento_label(form.tipo_procedimento) + " nº " + form.numero_procedimento;

    std::string html;
    if (!intro_template.empty()) {
        std::string text = intro_template;
        text = replace_marker(text, "$$DATA_EXTENSO$$",  data_extenso);
        text = replace_marker(text, "$$NOME_DELEGADO$$", form.nome_delegado);
        text = replace_marker(text, "$$PROCEDIMENTO$$",  procedimento_str);
        html = "  <p class=\"justified\">" + document_html::escape_html(text) + "</p>\n\n";
    } else {
        html = "  <p class=\"justified\">Por meio deste documento, aos ";
        html += document_html::escape_html(data_extenso);
        html += ", por determinação do Excelentíssimo Senhor Delegado de Polícia Civil, ";
        html += document_html::escape_html(form.nome_delegado);
        html += ", procedemos à transcrição, DE FORMA RESUMIDA E NÃO NECESSARIAMENTE COM AS MESMAS PALAVRAS, do teor das oitivas colhidas no âmbito ";
        html += document_html::escape_html(procedimento_str);
        html += ", na seguinte ordem cronológica:</p>\n\n";
    }

    const auto indices = sorted_general_entry_indices(form.oitivas);
    for (std::size_t sorted_index = 0; sorted_index < indices.size(); ++sorted_index) {
        const auto& entry = form.oitivas[indices[sorted_index]];
        const std::string qualificacao = entry.qualificacao.empty() ? qualificacao_padrao(entry.tipo_oitiva) : entry.qualificacao;

        html += "  <div class=\"oitiva\">\n";
        html += "    <div class=\"oitiva-title\">";
        html += document_html::escape_html(roman_numeral(sorted_index + 1));
        html += " &ndash; <span class=\"texto\">";
        if (!oitiva_template.empty()) {
            std::string t = oitiva_template;
            t = replace_marker(t, "$$NOME_PARTE$$",    entry.nome_parte);
            t = replace_marker(t, "$$QUALIFICACAO$$",  qualificacao);
            t = replace_marker(t, "$$DATA_OITIVA$$",   entry.data_oitiva);
            html += document_html::escape_html(t);
        } else {
            html += document_html::escape_html(entry.nome_parte);
            html += " (";
            html += document_html::escape_html(qualificacao);
            html += "), ouvido(a) no dia ";
            html += document_html::escape_html(entry.data_oitiva);
            html += ":";
        }
        html += "</span></div>\n";
        html += "    <div class=\"transcricao-box\">\n";
        html += document_html::paragraphs_from_wrapped_text(entry.relato, "      ");
        html += "    </div>\n";
        html += "  </div>\n\n";
    }

    html += "  <p class=\"closing\">";
    html += document_html::escape_html(closing_text.empty()
        ? "Nada mais havendo a constar, procedo ao encerramento das presentes transcrições, que vai devidamente assinado, na forma da Lei."
        : closing_text);
    html += "</p>\n\n";
    html += "  <div class=\"signature\">\n    <div class=\"name\">";
    html += document_html::escape_html(form.nome_responsavel);
    html += "</div>\n    <div>";
    html += document_html::escape_html(form.cargo_responsavel);
    html += "</div>\n  </div>\n";
    return html;
}


std::filesystem::path database_path() {
    return sto::files::executable_directory() / "data" / "sto.db";
}

std::filesystem::path documents_temp_directory() {
    return document_html::temporary_directory();
}

enum class Document { dashboard, auto_transcricao, auto_transcricao_geral };
enum class CalendarTarget { auto_transcricao, auto_transcricao_geral };
enum class Status { idle, success, failed };

struct State {
    Document document = Document::dashboard;
    sto::database::DocumentsRepository repository;
    bool initialized = false;
    bool database_ready = false;
    long long last_session_id_transcricao = 0;
    long long last_session_id_geral = 0;

    std::array<char, 256> nome_delegado{};
    int tipo_oitiva = 0;
    std::array<char, 256> nome_parte{};
    std::array<char, 11> data_oitiva{};
    int tipo_procedimento = 0;
    std::array<char, 64> numero_procedimento{};
    std::array<char, 256> local_depoimento_especial{};
    std::array<char, 32768> relato{};
    std::array<char, 256> nome_responsavel{};
    std::array<char, 128> cargo_responsavel{};

    int general_tipo_oitiva = 0;
    std::array<char, 256> general_nome_parte{};
    std::array<char, 128> general_qualificacao{};
    std::array<char, 11> general_data_oitiva{};
    std::array<char, 32768> general_relato{};
    std::vector<GeneralTranscriptionEntry> general_oitivas;
    std::size_t next_general_order = 0;
    int general_selected_oitiva = -1;
    int general_delete_candidate = -1;
    bool general_queue_collapsed = false;

    std::chrono::year_month calendar_view{std::chrono::year{2024}, std::chrono::month{1}};
    CalendarTarget calendar_target = CalendarTarget::auto_transcricao;

    std::string validation_error;
    bool duplicate_relato_confirmation = false;
    std::string duplicate_relato_warning;

    Status status = Status::idle;
    std::string message;
    std::chrono::steady_clock::time_point message_shown_at;
    std::filesystem::path html_path;

    std::array<char, 512>  tmpl_header_text{};
    std::array<char, 256>  tmpl_footer_text{};
    std::array<char, 4096> tmpl_intro_auto{};
    std::array<char, 4096> tmpl_intro_geral{};
    std::array<char, 1024> tmpl_closing_auto{};
    std::array<char, 1024> tmpl_closing_geral{};
    std::array<char, 512>  tmpl_oitiva_geral{};
    std::array<char, 256>  tmpl_audio_title{};
    std::array<char, 4096> tmpl_audio_intro{};
    std::array<char, 512>  tmpl_audio_entry_title{};
    std::array<char, 1024> tmpl_audio_closing{};
    std::array<char, 2048> tmpl_audio_note_whisper{};
    std::array<char, 2048> tmpl_audio_note_model{};
    std::string            tmpl_logo_path;
    int                    tmpl_selector = 0;

    void initialize() {
        if (initialized) return;
        initialized = true;
        std::string error;
        database_ready = repository.initialize(database_path(), error);
        if (database_ready) {
            const auto preferences = repository.load(error);
            std::snprintf(nome_delegado.data(), nome_delegado.size(), "%s", preferences.nome_delegado.c_str());
            std::snprintf(nome_responsavel.data(), nome_responsavel.size(), "%s", preferences.nome_responsavel.c_str());
            std::snprintf(cargo_responsavel.data(), cargo_responsavel.size(), "%s", preferences.cargo_responsavel.c_str());
            std::snprintf(local_depoimento_especial.data(), local_depoimento_especial.size(), "%s", preferences.local_depoimento_especial.c_str());

            const auto tmpl = repository.load_template(error);
            tmpl_logo_path = tmpl.logo_path;
            auto cp = [](auto& arr, const std::string& s) {
                std::snprintf(arr.data(), arr.size(), "%s", s.c_str());
            };
            cp(tmpl_header_text,   tmpl.header_text);
            cp(tmpl_footer_text,   tmpl.footer_text);
            cp(tmpl_intro_auto,    tmpl.intro_auto);
            cp(tmpl_intro_geral,   tmpl.intro_geral);
            cp(tmpl_closing_auto,  tmpl.closing_auto);
            cp(tmpl_closing_geral, tmpl.closing_geral);
            cp(tmpl_oitiva_geral,  tmpl.oitiva_geral);
            cp(tmpl_audio_title, tmpl.audio_title);
            cp(tmpl_audio_intro, tmpl.audio_intro);
            cp(tmpl_audio_entry_title, tmpl.audio_entry_title);
            cp(tmpl_audio_closing, tmpl.audio_closing);
            cp(tmpl_audio_note_whisper, tmpl.audio_note_whisper);
            cp(tmpl_audio_note_model, tmpl.audio_note_model);
        }
        const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
        const std::chrono::year_month_day ymd{today};
        calendar_view = ymd.year() / ymd.month();

        std::error_code cleanup_error;
        std::filesystem::remove_all(documents_temp_directory(), cleanup_error);
    }

    std::array<char, 11>& active_calendar_buffer() {
        return calendar_target == CalendarTarget::auto_transcricao ? data_oitiva : general_data_oitiva;
    }

    void open_calendar(CalendarTarget target) {
        calendar_target = target;
        if (const auto date = sto::text::parse_date_ddmmyyyy(active_calendar_buffer().data())) {
            calendar_view = date->year() / date->month();
        }
    }

    void set_calendar_date(unsigned day, unsigned month, int year) {
        auto& buffer = active_calendar_buffer();
        std::snprintf(buffer.data(), buffer.size(), "%02u/%02u/%04d", day, month, year);
    }

    void reset_status() {
        status = Status::idle;
        validation_error.clear();
    }
};

State& state() {
    static State instance;
    instance.initialize();
    return instance;
}


void generate_document(State& module, const FormData& form) {
    const auto assets_directory = sto::files::executable_directory() / "assets" / "documents";
    document_html::DocumentAssets assets;
    std::string error;
    if (!document_html::load_default_assets(assets_directory, assets, error)) {
        module.status = Status::failed;
        module.message = error;
        module.message_shown_at = std::chrono::steady_clock::now();
        return;
    }

    std::string date_for_filename = form.data_oitiva;
    std::ranges::replace(date_for_filename, '/', '-');
    const std::string sigla_s = procedure_sigla(form.tipo_procedimento);
    std::string browser_title = "Auto de Transcricao";
    if (!sigla_s.empty()) browser_title += " " + sigla_s;
    if (!form.numero_procedimento.empty()) browser_title += " " + form.numero_procedimento;
    if (!form.nome_parte.empty()) browser_title += " - " + form.nome_parte;
    if (!date_for_filename.empty()) browser_title += " - " + date_for_filename;

    if (!module.tmpl_logo_path.empty()) {
        std::ifstream logo_file(module.tmpl_logo_path, std::ios::binary);
        if (logo_file) {
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(logo_file)), {});
            if (!bytes.empty()) assets.logo_base64 = document_html::base64_encode(bytes);
        }
    }

    document_html::PagedDocument doc;
    doc.browser_title  = browser_title;
    doc.document_title = "AUTO DE TRANSCRIÇÃO";
    doc.header_html    = header_text_to_html(module.tmpl_header_text.data());
    doc.footer_html    = footer_text_to_html(module.tmpl_footer_text.data());
    doc.extra_style    = auto_transcricao_style();
    doc.body_html      = build_auto_transcricao_body(form,
        module.tmpl_intro_auto.data(), module.tmpl_closing_auto.data());
    const std::string html = document_html::build_paged_html(doc, assets);

    const auto temp_directory = documents_temp_directory();
    std::error_code ec;
    if (!module.html_path.empty()) std::filesystem::remove(module.html_path, ec);

    const std::wstring sigla_sw = widen(sigla_s);
    const std::wstring numero_sw = sto::files::sanitize_stem(widen(form.numero_procedimento));
    std::wstring filename = L"Auto de Transcricao";
    if (!sigla_sw.empty()) filename += L" " + sigla_sw;
    if (!numero_sw.empty()) filename += L" " + numero_sw;
    filename += L" - " + sto::files::sanitize_stem(widen(form.nome_parte));
    filename += L" - " + widen(date_for_filename) + L".html";
    const auto html_path = temp_directory / filename;

    if (!document_html::write_html_file(html_path, html, error)) {
        module.status = Status::failed;
        module.message = error;
        module.message_shown_at = std::chrono::steady_clock::now();
        return;
    }

    if (!document_html::open_in_default_browser(html_path, error)) {
        module.status = Status::failed;
        module.message = error;
        module.message_shown_at = std::chrono::steady_clock::now();
        return;
    }

    if (module.database_ready) {
        const sto::database::DocumentPreferences preferences{
            form.nome_delegado,
            form.nome_responsavel,
            form.cargo_responsavel,
            form.local_depoimento_especial
        };
        module.repository.save(preferences, error);
    }

    module.html_path = html_path;
    module.status = Status::success;
    module.message = "Documento aberto no navegador. Use o botão de impressão no canto inferior direito (ou Ctrl+P) para salvar como PDF.";
    module.message_shown_at = std::chrono::steady_clock::now();
}

void generate_document(State& module, const GeneralFormData& form) {
    const auto assets_directory = sto::files::executable_directory() / "assets" / "documents";
    document_html::DocumentAssets assets;
    std::string error;
    if (!document_html::load_default_assets(assets_directory, assets, error)) {
        module.status = Status::failed;
        module.message = error;
        module.message_shown_at = std::chrono::steady_clock::now();
        return;
    }

    const std::string sigla_g = procedure_sigla(form.tipo_procedimento);
    const std::string numero_g = form.numero_procedimento.empty() ? "sem numero" : form.numero_procedimento;
    std::string browser_title_g = "Auto de Transcricao Geral";
    if (!sigla_g.empty()) browser_title_g += " " + sigla_g;
    browser_title_g += " " + numero_g;

    if (!module.tmpl_logo_path.empty()) {
        std::ifstream logo_file(module.tmpl_logo_path, std::ios::binary);
        if (logo_file) {
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(logo_file)), {});
            if (!bytes.empty()) assets.logo_base64 = document_html::base64_encode(bytes);
        }
    }

    document_html::PagedDocument doc_g;
    doc_g.browser_title  = browser_title_g;
    doc_g.document_title = "AUTO DE TRANSCRIÇÃO";
    doc_g.header_html    = header_text_to_html(module.tmpl_header_text.data());
    doc_g.footer_html    = footer_text_to_html(module.tmpl_footer_text.data());
    doc_g.extra_style    = auto_transcricao_geral_style();
    doc_g.body_html      = build_auto_transcricao_geral_body(form,
        module.tmpl_intro_geral.data(), module.tmpl_closing_geral.data(),
        module.tmpl_oitiva_geral.data());
    const std::string html = document_html::build_paged_html(doc_g, assets);

    const auto temp_directory = documents_temp_directory();
    std::error_code ec;
    if (!module.html_path.empty()) std::filesystem::remove(module.html_path, ec);

    const std::wstring sigla_gw = widen(sigla_g);
    const std::wstring numero_gw = sto::files::sanitize_stem(widen(numero_g));
    std::wstring filename = L"Auto de Transcricao Geral";
    if (!sigla_gw.empty()) filename += L" " + sigla_gw;
    filename += L" " + numero_gw + L".html";
    const auto html_path = temp_directory / filename;

    if (!document_html::write_html_file(html_path, html, error)) {
        module.status = Status::failed;
        module.message = error;
        module.message_shown_at = std::chrono::steady_clock::now();
        return;
    }

    if (!document_html::open_in_default_browser(html_path, error)) {
        module.status = Status::failed;
        module.message = error;
        module.message_shown_at = std::chrono::steady_clock::now();
        return;
    }

    if (module.database_ready) {
        const sto::database::DocumentPreferences preferences{
            form.nome_delegado,
            form.nome_responsavel,
            form.cargo_responsavel,
            module.local_depoimento_especial.data()
        };
        module.repository.save(preferences, error);
    }

    module.html_path = html_path;
    module.status = Status::success;
    module.message = "Documento aberto no navegador. Use o botão de impressão no canto inferior direito (ou Ctrl+P) para salvar como PDF.";
    module.message_shown_at = std::chrono::steady_clock::now();
}

void render_dashboard(State& module) {
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "Geração de Documentos");
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::Spacing();
    wrapped_text(kMuted, "Selecione um modelo para preencher e gerar o documento.");
    ImGui::Spacing();

    struct Card { Document document; const char* icon; const char* title; const char* description; ImVec4 color; };
    constexpr std::array cards = {
        Card{Document::auto_transcricao, kIconPdf, "AUTO DE TRANSCRIÇÃO", "Transcrição resumida de declaração, depoimento ou interrogatório.", kBlue},
        Card{Document::auto_transcricao_geral, kIconPdf, "AUTO DE TRANSCRIÇÃO GERAL", "Transcrições reunidas de todas as oitivas do procedimento, em ordem cronológica.", kGreen},
    };
    if (ImGui::BeginTable("document-cards", 3, ImGuiTableFlags_SizingStretchSame)) {
        for (const auto& card : cards) {
            ImGui::TableNextColumn();
            ImGui::BeginChild(card.title, {0.0F, 176.0F}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
            accent_line(42.0F, card.color);
            ImGui::Spacing();
            ImGui::SetWindowFontScale(1.18F);
            ImGui::TextColored(card.color, "%s", card.icon);
            ImGui::SameLine();
            ImGui::TextColored(kWhite, "%s", card.title);
            ImGui::SetWindowFontScale(1.00F);
            ImGui::Spacing();
            wrapped_text(kMuted, card.description);
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 44.0F);
            if (colored_button("Abrir##" + std::string(card.title), {-1.0F, 32.0F}, card.color)) {
                module.document = card.document;
                module.reset_status();
                const auto snap = sto::modules::transcription::get_selected_session_snapshot();
                if (snap.has_session) {
                    if (card.document == Document::auto_transcricao) {
                        std::snprintf(module.nome_parte.data(), module.nome_parte.size(), "%s", snap.party_name.c_str());
                        module.tipo_oitiva = std::max(0, snap.hearing_type - 1);
                        std::snprintf(module.data_oitiva.data(), module.data_oitiva.size(), "%s", snap.date_oitiva.c_str());
                    } else if (card.document == Document::auto_transcricao_geral && module.general_oitivas.empty()) {
                        GeneralTranscriptionEntry entry;
                        entry.tipo_oitiva = std::max(0, snap.hearing_type - 1);
                        entry.nome_parte = snap.party_name;
                        entry.qualificacao = qualificacao_padrao(entry.tipo_oitiva);
                        entry.data_oitiva = snap.date_oitiva;
                        entry.insertion_order = module.next_general_order++;
                        module.general_oitivas.push_back(std::move(entry));
                        module.general_selected_oitiva = 0;
                        module.general_tipo_oitiva = snap.hearing_type;
                        std::snprintf(module.general_nome_parte.data(), module.general_nome_parte.size(), "%s", snap.party_name.c_str());
                        const std::string qual = qualificacao_padrao(snap.hearing_type);
                        std::snprintf(module.general_qualificacao.data(), module.general_qualificacao.size(), "%s", qual.c_str());
                        std::snprintf(module.general_data_oitiva.data(), module.general_data_oitiva.size(), "%s", snap.date_oitiva.c_str());
                    }
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndTable();
    }
}

void render_calendar_popup(State& module) {
    if (!ImGui::BeginPopup("calendar-popup")) return;
    namespace chrono = std::chrono;

    const int year = static_cast<int>(module.calendar_view.year());
    const unsigned month = static_cast<unsigned>(module.calendar_view.month());

    if (ImGui::Button("<", {28.0F, 0.0F})) module.calendar_view -= chrono::months{1};
    ImGui::SameLine();
    const std::string month_label = capitalize(sto::text::month_name_pt(month)) + " " + std::to_string(year);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(month_label.c_str()).x) * 0.5F);
    ImGui::TextColored(kWhite, "%s", month_label.c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 28.0F - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button(">", {28.0F, 0.0F})) module.calendar_view += chrono::months{1};
    ImGui::Spacing();

    if (ImGui::BeginTable("calendar-grid", 7, ImGuiTableFlags_SizingFixedFit)) {
        for (const char* weekday : {"D", "S", "T", "Q", "Q", "S", "S"}) {
            ImGui::TableNextColumn();
            ImGui::TextColored(kMuted, "%s", weekday);
        }

        const chrono::year_month_day first_day{module.calendar_view / chrono::day{1}};
        const chrono::weekday first_weekday{chrono::sys_days{first_day}};
        const chrono::year_month_day_last last_day{module.calendar_view / chrono::last};
        const unsigned days_in_month = static_cast<unsigned>(last_day.day());

        for (unsigned lead = 0; lead < first_weekday.c_encoding(); ++lead) ImGui::TableNextColumn();
        for (unsigned day = 1; day <= days_in_month; ++day) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(day));
            char day_label[4];
            std::snprintf(day_label, sizeof(day_label), "%u", day);
            if (ImGui::Button(day_label, {30.0F, 26.0F})) {
                module.set_calendar_date(day, month, year);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndPopup();
}

void start_generation(State& module) {
    module.validation_error.clear();

    if (module.nome_parte[0] == '\0') {
        module.validation_error = "Informe o nome da parte.";
        return;
    }
    if (module.relato[0] == '\0') {
        module.validation_error = "Informe o relato da oitiva.";
        return;
    }
    if (!sto::text::parse_date_ddmmyyyy(module.data_oitiva.data())) {
        module.validation_error = "Informe uma data válida no formato DD/MM/AAAA.";
        return;
    }
    if (module.tipo_oitiva == kDepoimentoEspecialIndex && module.local_depoimento_especial[0] == '\0') {
        module.validation_error = "Informe o local onde foi realizado o depoimento especial.";
        return;
    }

    const FormData form{
        module.nome_delegado.data(), module.tipo_oitiva, module.nome_parte.data(),
        module.data_oitiva.data(), module.tipo_procedimento, module.numero_procedimento.data(),
        module.local_depoimento_especial.data(), module.relato.data(), module.nome_responsavel.data(), module.cargo_responsavel.data()
    };

    generate_document(module, form);
}

bool auto_transcricao_has_content(const State& module) {
    return module.tipo_oitiva != 0
        || module.nome_parte[0] != '\0'
        || module.data_oitiva[0] != '\0'
        || module.tipo_procedimento != 0
        || module.numero_procedimento[0] != '\0'
        || module.relato[0] != '\0';
}

void clear_auto_transcricao_content(State& module) {
    module.tipo_oitiva = 0;
    module.nome_parte.fill('\0');
    module.data_oitiva.fill('\0');
    module.tipo_procedimento = 0;
    module.numero_procedimento.fill('\0');
    module.relato.fill('\0');
    module.reset_status();
}

void clear_general_current_oitiva(State& module, bool reset_tipo) {
    if (reset_tipo) {
        module.general_tipo_oitiva = 0;
        module.general_qualificacao.fill('\0');
    }
    module.general_nome_parte.fill('\0');
    module.general_data_oitiva.fill('\0');
    module.general_relato.fill('\0');
}

bool has_selected_general_oitiva(const State& module) {
    return module.general_selected_oitiva >= 0
        && static_cast<std::size_t>(module.general_selected_oitiva) < module.general_oitivas.size();
}

void load_general_oitiva_to_editor(State& module, std::size_t index) {
    if (index >= module.general_oitivas.size()) return;
    const auto& entry = module.general_oitivas[index];
    module.general_selected_oitiva = static_cast<int>(index);
    module.general_tipo_oitiva = entry.tipo_oitiva;
    std::snprintf(module.general_nome_parte.data(), module.general_nome_parte.size(), "%s", entry.nome_parte.c_str());
    std::snprintf(module.general_qualificacao.data(), module.general_qualificacao.size(), "%s", entry.qualificacao.c_str());
    std::snprintf(module.general_data_oitiva.data(), module.general_data_oitiva.size(), "%s", entry.data_oitiva.c_str());
    std::snprintf(module.general_relato.data(), module.general_relato.size(), "%s", entry.relato.c_str());
    module.reset_status();
}

void sync_general_editor_to_selected(State& module) {
    if (!has_selected_general_oitiva(module)) return;
    auto& entry = module.general_oitivas[static_cast<std::size_t>(module.general_selected_oitiva)];
    entry.tipo_oitiva = module.general_tipo_oitiva;
    entry.nome_parte = module.general_nome_parte.data();
    entry.qualificacao = module.general_qualificacao[0] != '\0'
        ? module.general_qualificacao.data()
        : qualificacao_padrao(module.general_tipo_oitiva);
    entry.data_oitiva = module.general_data_oitiva.data();
    entry.relato = module.general_relato.data();
}

void create_general_oitiva_draft(State& module) {
    GeneralTranscriptionEntry entry;
    entry.tipo_oitiva = 0;
    entry.qualificacao = qualificacao_padrao(entry.tipo_oitiva);
    entry.insertion_order = module.next_general_order++;
    module.general_oitivas.push_back(std::move(entry));
    load_general_oitiva_to_editor(module, module.general_oitivas.size() - 1);
}

void delete_general_oitiva(State& module, std::size_t index) {
    if (index >= module.general_oitivas.size()) return;
    module.general_oitivas.erase(module.general_oitivas.begin() + static_cast<std::ptrdiff_t>(index));
    if (module.general_oitivas.empty()) {
        module.general_selected_oitiva = -1;
        clear_general_current_oitiva(module, true);
    } else if (module.general_selected_oitiva == static_cast<int>(index)) {
        load_general_oitiva_to_editor(module, std::min(index, module.general_oitivas.size() - 1));
    } else if (module.general_selected_oitiva > static_cast<int>(index)) {
        --module.general_selected_oitiva;
    }
    module.general_delete_candidate = -1;
    module.reset_status();
}

bool auto_transcricao_geral_has_content(const State& module) {
    return module.tipo_procedimento != 0
        || module.numero_procedimento[0] != '\0'
        || !module.general_oitivas.empty()
        || module.general_nome_parte[0] != '\0'
        || module.general_data_oitiva[0] != '\0'
        || module.general_relato[0] != '\0';
}

void clear_auto_transcricao_geral_content(State& module) {
    module.tipo_procedimento = 0;
    module.numero_procedimento.fill('\0');
    module.general_oitivas.clear();
    module.next_general_order = 0;
    module.general_selected_oitiva = -1;
    module.general_delete_candidate = -1;
    clear_general_current_oitiva(module, true);
    module.reset_status();
}

// Checks all oitiva pairs for identical or near-identical relatos (one
// contains the other after normalisation). Returns a human-readable warning
// string listing the duplicate pairs, or an empty string when all clear.
std::string find_duplicate_relatos(const std::vector<GeneralTranscriptionEntry>& oitivas) {
    const auto indices = sorted_general_entry_indices(oitivas);
    const std::size_t n = indices.size();

    auto normalize = [](const std::string& s) {
        std::string r;
        for (unsigned char c : s) {
            if (std::isspace(c)) { if (!r.empty() && r.back() != ' ') r += ' '; }
            else r += static_cast<char>(std::tolower(c));
        }
        while (!r.empty() && r.back() == ' ') r.pop_back();
        return r;
    };

    std::vector<std::string> norm(n);
    for (std::size_t i = 0; i < n; ++i) norm[i] = normalize(oitivas[indices[i]].relato);

    std::string warning;
    for (std::size_t i = 0; i < n; ++i) {
        if (norm[i].size() < 30) continue;
        for (std::size_t j = i + 1; j < n; ++j) {
            if (norm[j].size() < 30) continue;
            const bool identical = (norm[i] == norm[j]);
            const bool contained = !identical
                && norm[i].size() >= 80 && norm[j].size() >= 80
                && (norm[i].find(norm[j]) != std::string::npos
                    || norm[j].find(norm[i]) != std::string::npos);
            if (identical || contained) {
                if (!warning.empty()) warning += '\n';
                warning += "Oitivas " + roman_numeral(i + 1) + " e " + roman_numeral(j + 1)
                    + (identical ? ": relatos idênticos." : ": um relato está contido no outro.");
            }
        }
    }
    return warning;
}

void start_general_generation(State& module, bool bypass_duplicate_check = false) {
    module.validation_error.clear();
    sync_general_editor_to_selected(module);

    if (module.numero_procedimento[0] == '\0') {
        module.validation_error = "Informe o número do procedimento.";
        return;
    }
    if (module.general_oitivas.empty()) {
        module.validation_error = "Adicione pelo menos uma oitiva à lista.";
        return;
    }

    const auto indices = sorted_general_entry_indices(module.general_oitivas);
    for (std::size_t order = 0; order < indices.size(); ++order) {
        const std::size_t index = indices[order];
        const auto& entry = module.general_oitivas[index];
        const std::string ordem = roman_numeral(order + 1);
        if (entry.nome_parte.empty()) {
            load_general_oitiva_to_editor(module, index);
            module.validation_error = "Informe o nome da pessoa ouvida na oitiva " + ordem + ".";
            return;
        }
        if (!sto::text::parse_date_ddmmyyyy(entry.data_oitiva)) {
            load_general_oitiva_to_editor(module, index);
            module.validation_error = "Informe uma data válida na oitiva " + ordem + ".";
            return;
        }
        if (entry.relato.empty()) {
            load_general_oitiva_to_editor(module, index);
            module.validation_error = "Informe a transcrição/resumo da oitiva " + ordem + ".";
            return;
        }
    }

    if (!bypass_duplicate_check) {
        const std::string dup = find_duplicate_relatos(module.general_oitivas);
        if (!dup.empty()) {
            module.duplicate_relato_warning = dup;
            module.duplicate_relato_confirmation = true;
            return;
        }
    }

    const GeneralFormData form{
        module.nome_delegado.data(),
        module.tipo_procedimento,
        module.numero_procedimento.data(),
        module.general_oitivas,
        module.nome_responsavel.data(),
        module.cargo_responsavel.data()
    };

    generate_document(module, form);
}

void render_result(State& module) {
    if (module.status == Status::idle) return;

    const auto elapsed = std::chrono::steady_clock::now() - module.message_shown_at;
    if (elapsed > std::chrono::seconds{5}) {
        module.reset_status();
        return;
    }

    ImGui::Spacing();
    const ImVec4 text_color = module.status == Status::failed ? kError : kSuccess;
    wrapped_text(text_color, module.message);
}

// Side-by-side column geometry for a two-field row. Both fields are rendered
// from the same starting Y position (rather than relying on table row
// alignment), so a longer label/value in one column cannot push it out of
// vertical sync with the other.
struct TwoColumnLayout {
    ImVec2 left_pos;
    float left_width;
    ImVec2 right_pos;
    float right_width;
};

TwoColumnLayout two_column_layout(float left_weight, float right_weight) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 start = ImGui::GetCursorPos();
    const float total_width = ImGui::GetContentRegionAvail().x;
    const float gap = style.ItemSpacing.x;
    const float left_width = (total_width - gap) * left_weight / (left_weight + right_weight);
    const float right_width = total_width - gap - left_width;
    return {start, left_width, {start.x + left_width + gap, start.y}, right_width};
}

void end_two_columns(const TwoColumnLayout& layout, float left_bottom) {
    ImGui::SetCursorPos({layout.left_pos.x, std::max(left_bottom, ImGui::GetCursorPosY())});
}

// Renders a "label above input" pair starting at `pos`, sizing the widget to
// `width`. The cursor is repositioned to `pos.x` before each item so the
// column stays put regardless of where the previous item left the cursor.
void labeled_input_at(const char* label_text, const char* widget_id, char* buffer, std::size_t buffer_size, ImVec2 pos, float width) {
    ImGui::SetCursorPos(pos);
    ImGui::TextColored(kMuted, "%s", label_text);
    ImGui::SetCursorPos({pos.x, ImGui::GetCursorPosY()});
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(widget_id, buffer, buffer_size);
}

void labeled_combo_at(const char* label_text, const char* widget_id, int* current_item, const char* const items[], int items_count, ImVec2 pos, float width) {
    ImGui::SetCursorPos(pos);
    ImGui::TextColored(kMuted, "%s", label_text);
    ImGui::SetCursorPos({pos.x, ImGui::GetCursorPosY()});
    ImGui::SetNextItemWidth(width);
    ImGui::Combo(widget_id, current_item, items, items_count);
}

void center_popup_buttons(float total_width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > total_width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - total_width) * 0.5F);
    }
}

bool transparent_trash_button(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1.0F, 1.0F, 1.0F, 0.06F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{1.0F, 1.0F, 1.0F, 0.10F});
    ImGui::PushStyleColor(ImGuiCol_Text, kError);
    const bool clicked = ImGui::SmallButton(id);
    ImGui::PopStyleColor(4);
    return clicked;
}

void render_general_oitivas_list(State& module, const ImVec2& size) {
    bool open_delete_popup = false;
    const bool collapsed = module.general_queue_collapsed;
    ImGui::BeginChild("general-oitivas-list", size, false);
    if (module.general_oitivas.empty()) {
        if (!collapsed) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const char* message = "Nenhuma oitiva adicionada.";
            const char* hint = "Use \"Adicionar oitiva\" para criar o primeiro rascunho.";
            ImGui::SetCursorPos({
                std::max(0.0F, (avail.x - ImGui::CalcTextSize(message).x) * 0.5F),
                std::max(0.0F, avail.y * 0.5F - ImGui::GetTextLineHeightWithSpacing())
            });
            ImGui::TextColored(kMuted, "%s", message);
            ImGui::SetCursorPosX(std::max(0.0F, (avail.x - ImGui::CalcTextSize(hint).x) * 0.5F));
            ImGui::TextColored(kMuted, "%s", hint);
        }
    } else {
        constexpr float kRowHeight = 58.0F;
        constexpr float kRowHeightCollapsed = 44.0F;
        constexpr float kBadgeSize = 32.0F;
        constexpr float kBadgeSizeCollapsed = 28.0F;
        const float row_height = collapsed ? kRowHeightCollapsed : kRowHeight;
        const float badge_size = collapsed ? kBadgeSizeCollapsed : kBadgeSize;
        const ImGuiStyle& style = ImGui::GetStyle();
        const float row_left_x = ImGui::GetCursorScreenPos().x;
        const float row_right_x = row_left_x + ImGui::GetContentRegionAvail().x;
        const float trash_button_width = ImGui::CalcTextSize(kIconTrash).x + style.FramePadding.x * 2.0F;
        const float trash_x = row_right_x - trash_button_width;
        const auto indices = sorted_general_entry_indices(module.general_oitivas);
        for (std::size_t order = 0; order < indices.size(); ++order) {
            const std::size_t entry_index = indices[order];
            const auto& entry = module.general_oitivas[entry_index];
            const std::string ordem = roman_numeral(order + 1);
            const std::string qualificacao = entry.qualificacao.empty() ? qualificacao_padrao(entry.tipo_oitiva) : entry.qualificacao;
            const std::string name = entry.nome_parte.empty() ? "Rascunho sem nome" : entry.nome_parte;
            const std::string date = entry.data_oitiva.empty() ? "sem data" : entry.data_oitiva;
            const std::string subtitle = capitalize(oitiva_lowercase(entry.tipo_oitiva)) + "  ·  " + qualificacao + "  ·  " + date;
            const bool selected = module.general_selected_oitiva == static_cast<int>(entry_index);

            ImGui::PushID(static_cast<int>(entry_index));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4{0.12F, 0.26F, 0.18F, 1.0F});
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.14F, 0.32F, 0.22F, 1.0F});
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4{0.12F, 0.36F, 0.24F, 1.0F});
            if (ImGui::Selectable("##oitiva-draft", selected, ImGuiSelectableFlags_AllowOverlap, {0.0F, row_height})) {
                load_general_oitiva_to_editor(module, entry_index);
            }
            ImGui::PopStyleColor(3);

            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImDrawList* draw = ImGui::GetWindowDrawList();

            if (selected) {
                draw->AddRectFilled({min.x, min.y}, {min.x + 4.0F, max.y}, ImGui::GetColorU32(kBlue));
            }

            const float badge_x = collapsed ? min.x + ((max.x - min.x) - badge_size) * 0.5F : min.x + 11.0F;
            const ImVec2 badge_min{badge_x, min.y + (row_height - badge_size) * 0.5F};
            const ImVec2 badge_max{badge_min.x + badge_size, badge_min.y + badge_size};
            draw->AddRectFilled(badge_min, badge_max, ImGui::GetColorU32(kBlue), 6.0F);
            const ImVec2 ordem_size = ImGui::CalcTextSize(ordem.c_str());
            draw->AddText({badge_min.x + (badge_size - ordem_size.x) * 0.5F, badge_min.y + (badge_size - ordem_size.y) * 0.5F}, ImGui::GetColorU32(kWhite), ordem.c_str());

            if (collapsed) {
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", name.c_str(), subtitle.c_str());
            } else {
                const float text_x = badge_max.x + 12.0F;
                draw->PushClipRect({text_x, min.y}, {trash_x - 8.0F, max.y}, true);
                draw->AddText({text_x, min.y + 10.0F}, ImGui::GetColorU32(kWhite), name.c_str());
                draw->AddText({text_x, min.y + 32.0F}, ImGui::GetColorU32(kMuted), subtitle.c_str());
                draw->PopClipRect();

                ImGui::SetCursorScreenPos({trash_x, min.y + (row_height - 22.0F) * 0.5F});
                if (transparent_trash_button(kIconTrash)) {
                    module.general_delete_candidate = static_cast<int>(entry_index);
                    open_delete_popup = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remover oitiva");
            }
            ImGui::SetCursorScreenPos({row_left_x, max.y + style.ItemSpacing.y});
            ImGui::PopID();
        }
        // The last row's SetCursorScreenPos() above moves the cursor past the
        // child's tracked content bounds; submit a zero-size item so ImGui
        // accounts for it (otherwise it logs a SetCursorScreenPos warning).
        ImGui::Dummy({0.0F, 0.0F});
    }
    ImGui::EndChild();

    if (open_delete_popup) ImGui::OpenPopup("Confirmar exclusão da oitiva");
    if (ImGui::BeginPopupModal("Confirmar exclusão da oitiva", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Deseja realmente excluir esta oitiva?");
        ImGui::TextColored(kSilver, "Esta ação remove apenas o rascunho da lista atual.");
        ImGui::Spacing();
        center_popup_buttons(130.0F + ImGui::GetStyle().ItemSpacing.x + 110.0F);
        if (colored_button(label(kIconTrash, "Excluir"), {130.0F, 36.0F}, kRed)) {
            if (module.general_delete_candidate >= 0) {
                delete_general_oitiva(module, static_cast<std::size_t>(module.general_delete_candidate));
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Manter", {110.0F, 36.0F})) {
            module.general_delete_candidate = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void render_auto_transcricao(State& module) {
    {
        const auto snap = sto::modules::transcription::get_selected_session_snapshot();
        if (snap.has_session && snap.session_id != module.last_session_id_transcricao) {
            module.last_session_id_transcricao = snap.session_id;
            if (module.relato[0] == '\0') {
                std::snprintf(module.nome_parte.data(), module.nome_parte.size(), "%s", snap.party_name.c_str());
                module.tipo_oitiva = std::max(0, snap.hearing_type - 1);
                std::snprintf(module.data_oitiva.data(), module.data_oitiva.size(), "%s", snap.date_oitiva.c_str());
            }
        }
    }
    if (colored_button(label(kIconBack, "Voltar"), {112.0F, 34.0F}, ImVec4{0.14F, 0.15F, 0.16F, 1.0F})) {
        module.document = Document::dashboard;
        module.reset_status();
    }
    ImGui::Spacing();

    accent_line(52.0F, kBlue);
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "Auto de Transcrição");
    if (sto::ui::heading_font()) ImGui::PopFont();
    wrapped_text(kMuted, "Preencha os dados da oitiva para gerar o documento, pronto para impressão ou exportação em PDF.");
    ImGui::Spacing();
    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    constexpr float kCalendarButtonWidth = 38.0F;
    const int tipo_oitiva_before = module.tipo_oitiva;

    // Row: Nome do delegado | Data da oitiva
    {
        const auto cols = two_column_layout(2.0F, 1.0F);
        labeled_input_at("Nome do delegado", "##nome-delegado", module.nome_delegado.data(), module.nome_delegado.size(), cols.left_pos, cols.left_width);
        const float left_bottom = ImGui::GetCursorPosY();

        ImGui::SetCursorPos(cols.right_pos);
        ImGui::TextColored(kMuted, "Data da oitiva");
        ImGui::SetCursorPos({cols.right_pos.x, ImGui::GetCursorPosY()});
        ImGui::SetNextItemWidth(std::max(1.0F, cols.right_width - (kCalendarButtonWidth + style.ItemSpacing.x)));
        ImGui::InputTextWithHint("##data-oitiva", "DD/MM/AAAA", module.data_oitiva.data(), module.data_oitiva.size(), ImGuiInputTextFlags_CallbackEdit, sto::text::date_mask_callback);
        ImGui::SameLine();
        if (ImGui::Button(kIconCalendar, {kCalendarButtonWidth, 0.0F})) {
            module.open_calendar(CalendarTarget::auto_transcricao);
            ImGui::OpenPopup("calendar-popup");
        }
        render_calendar_popup(module);

        end_two_columns(cols, left_bottom);
    }
    ImGui::Spacing();

    // Row: Tipo da oitiva | Nome da parte
    {
        const auto cols = two_column_layout(1.0F, 1.0F);
        labeled_combo_at("Tipo da oitiva", "##tipo-oitiva", &module.tipo_oitiva, kTiposOitiva.data(), static_cast<int>(kTiposOitiva.size()), cols.left_pos, cols.left_width);
        const float left_bottom = ImGui::GetCursorPosY();

        labeled_input_at("Nome da parte", "##nome-parte", module.nome_parte.data(), module.nome_parte.size(), cols.right_pos, cols.right_width);

        end_two_columns(cols, left_bottom);
    }
    ImGui::Spacing();

    if (tipo_oitiva_before != module.tipo_oitiva && module.tipo_oitiva == kDepoimentoEspecialIndex) {
        module.tipo_procedimento = kCartaPrecatoriaIndex;
    }
    const bool is_depoimento_especial = module.tipo_oitiva == kDepoimentoEspecialIndex;

    // Row: Tipo do procedimento | Número do procedimento
    {
        const auto cols = two_column_layout(1.0F, 1.0F);
        labeled_combo_at("Tipo do procedimento", "##tipo-procedimento", &module.tipo_procedimento, kTiposProcedimento.data(), static_cast<int>(kTiposProcedimento.size()), cols.left_pos, cols.left_width);
        const float left_bottom = ImGui::GetCursorPosY();

        labeled_input_at("Número do procedimento", "##numero-procedimento", module.numero_procedimento.data(), module.numero_procedimento.size(), cols.right_pos, cols.right_width);

        end_two_columns(cols, left_bottom);
    }
    ImGui::Spacing();

    if (is_depoimento_especial) {
        ImGui::TextColored(kMuted, "Local onde foi realizado");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("##local-depoimento-especial", module.local_depoimento_especial.data(), module.local_depoimento_especial.size());
        ImGui::Spacing();
    }

    // Relato — fills the remaining space, leaving room for the fields and
    // controls that come after it so the form fits the window without scrolling.
    ImGui::TextColored(kMuted, "Relato");
    // The validation/result message can wrap onto a second line in a narrow
    // window, so its reservation covers two lines rather than one.
    const float reserved_below_relato = style.ItemSpacing.y
        + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() + style.ItemSpacing.y
        + 3.0F + style.ItemSpacing.y
        + 46.0F + style.ItemSpacing.y
        + 36.0F + style.ItemSpacing.y
        + ImGui::GetTextLineHeightWithSpacing() * 2.0F;
    const float relato_min_height = ImGui::GetTextLineHeightWithSpacing() * 4.0F;
    const float relato_height = std::max(relato_min_height, ImGui::GetContentRegionAvail().y - reserved_below_relato);

    ImGui::InputTextMultiline(
        "##relato", module.relato.data(), module.relato.size(), {-1.0F, relato_height},
        ImGuiInputTextFlags_WordWrap
    );
    ImGui::Spacing();

    // Row: Nome do responsável pela assinatura | Cargo do responsável
    {
        const auto cols = two_column_layout(1.0F, 1.0F);
        labeled_input_at("Nome do responsável pela assinatura", "##nome-responsavel", module.nome_responsavel.data(), module.nome_responsavel.size(), cols.left_pos, cols.left_width);
        const float left_bottom = ImGui::GetCursorPosY();

        labeled_input_at("Cargo do responsável", "##cargo-responsavel", module.cargo_responsavel.data(), module.cargo_responsavel.size(), cols.right_pos, cols.right_width);

        end_two_columns(cols, left_bottom);
    }
    ImGui::Spacing();

    accent_line(ImGui::GetContentRegionAvail().x, kSilver);
    ImGui::Spacing();

    if (!module.validation_error.empty()) {
        wrapped_text(kError, module.validation_error);
        ImGui::Spacing();
    }

    if (colored_button(label(kIconPdf, "Gerar e abrir no navegador"), {-1.0F, 46.0F}, kGreen)) start_generation(module);
    ImGui::Spacing();
    if (colored_button(label(kIconTrash, "Limpar conteúdo"), {-1.0F, 36.0F}, kRed)) {
        if (auto_transcricao_has_content(module)) {
            ImGui::OpenPopup("Confirmar limpeza do conteúdo");
        } else {
            clear_auto_transcricao_content(module);
        }
    }
    if (ImGui::BeginPopupModal("Confirmar limpeza do conteúdo", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Deseja realmente limpar o conteúdo do formulário?");
        ImGui::TextColored(kSilver, "Todos os campos preenchidos serão apagados.");
        ImGui::Spacing();
        center_popup_buttons(130.0F + style.ItemSpacing.x + 110.0F);
        if (colored_button(label(kIconTrash, "Limpar"), {130.0F, 36.0F}, kRed)) {
            clear_auto_transcricao_content(module);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", {110.0F, 36.0F})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    render_result(module);
}

void render_auto_transcricao_geral(State& module) {
    {
        const auto snap = sto::modules::transcription::get_selected_session_snapshot();
        if (snap.has_session && snap.session_id != module.last_session_id_geral) {
            module.last_session_id_geral = snap.session_id;
            if (module.general_relato[0] == '\0') {
                module.general_tipo_oitiva = std::max(0, snap.hearing_type - 1);
                std::snprintf(module.general_nome_parte.data(), module.general_nome_parte.size(), "%s", snap.party_name.c_str());
                const std::string qual = qualificacao_padrao(module.general_tipo_oitiva);
                std::snprintf(module.general_qualificacao.data(), module.general_qualificacao.size(), "%s", qual.c_str());
                std::snprintf(module.general_data_oitiva.data(), module.general_data_oitiva.size(), "%s", snap.date_oitiva.c_str());
            }
        }
    }
    if (colored_button(label(kIconBack, "Voltar"), {112.0F, 34.0F}, ImVec4{0.14F, 0.15F, 0.16F, 1.0F})) {
        module.document = Document::dashboard;
        module.reset_status();
    }
    ImGui::Spacing();

    accent_line(52.0F, kGreen);
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "Auto de Transcrição Geral");
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    constexpr float kCalendarButtonWidth = 38.0F;
    const float total_width = ImGui::GetContentRegionAvail().x;
    const bool side_by_side = total_width >= 760.0F;
    constexpr float kQueueCollapsedWidth = 58.0F;
    const float right_width = side_by_side
        ? (module.general_queue_collapsed ? kQueueCollapsedWidth : std::clamp(total_width * 0.36F, 340.0F, 460.0F))
        : total_width;
    const float left_width = side_by_side ? std::max(360.0F, total_width - right_width - style.ItemSpacing.x) : total_width;

    ImGui::BeginChild("general-form-left", {left_width, side_by_side ? 0.0F : 520.0F}, false);
    ImGui::TextColored(kSilver, "Dados do procedimento");
    ImGui::Spacing();
    {
        const auto cols = two_column_layout(2.0F, 1.0F);
        labeled_input_at("Nome do delegado", "##geral-nome-delegado", module.nome_delegado.data(), module.nome_delegado.size(), cols.left_pos, cols.left_width);
        const float left_bottom = ImGui::GetCursorPosY();

        labeled_combo_at("Tipo do procedimento", "##geral-tipo-procedimento", &module.tipo_procedimento, kTiposProcedimento.data(), static_cast<int>(kTiposProcedimento.size()), cols.right_pos, cols.right_width);

        end_two_columns(cols, left_bottom);
    }
    ImGui::Spacing();

    {
        const auto cols = two_column_layout(1.0F, 1.0F);
        labeled_input_at("Número do procedimento", "##geral-numero-procedimento", module.numero_procedimento.data(), module.numero_procedimento.size(), cols.left_pos, cols.left_width);
        const float left_bottom = ImGui::GetCursorPosY();

        labeled_input_at("Nome do responsável pela assinatura", "##geral-nome-responsavel", module.nome_responsavel.data(), module.nome_responsavel.size(), cols.right_pos, cols.right_width);

        end_two_columns(cols, left_bottom);
    }
    ImGui::Spacing();

    ImGui::TextColored(kMuted, "Cargo do responsável");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputText("##geral-cargo-responsavel", module.cargo_responsavel.data(), module.cargo_responsavel.size());
    ImGui::Spacing();

    accent_line(ImGui::GetContentRegionAvail().x, kSilver);
    ImGui::Spacing();
    const bool editing_general_oitiva = has_selected_general_oitiva(module);
    ImGui::TextColored(kSilver, "Oitiva em edição");
    ImGui::Spacing();

    ImGui::BeginDisabled(!editing_general_oitiva);
    const int tipo_oitiva_before = module.general_tipo_oitiva;
    {
        const auto cols = two_column_layout(1.0F, 1.0F);
        labeled_combo_at("Tipo da oitiva", "##geral-tipo-oitiva", &module.general_tipo_oitiva, kTiposOitiva.data(), static_cast<int>(kTiposOitiva.size()), cols.left_pos, cols.left_width);
        const float left_bottom = ImGui::GetCursorPosY();

        labeled_input_at("Nome da pessoa ouvida", "##geral-nome-parte", module.general_nome_parte.data(), module.general_nome_parte.size(), cols.right_pos, cols.right_width);

        end_two_columns(cols, left_bottom);
    }
    if (tipo_oitiva_before != module.general_tipo_oitiva
        && (module.general_qualificacao[0] == '\0'
            || std::strcmp(module.general_qualificacao.data(), qualificacao_padrao(tipo_oitiva_before).c_str()) == 0)) {
        const std::string padrao = qualificacao_padrao(module.general_tipo_oitiva);
        std::snprintf(module.general_qualificacao.data(), module.general_qualificacao.size(), "%s", padrao.c_str());
    }
    ImGui::Spacing();

    {
        const auto cols = two_column_layout(1.0F, 2.0F);
        ImGui::SetCursorPos(cols.left_pos);
        ImGui::TextColored(kMuted, "Data da oitiva");
        ImGui::SetCursorPos({cols.left_pos.x, ImGui::GetCursorPosY()});
        ImGui::SetNextItemWidth(std::max(1.0F, cols.left_width - (kCalendarButtonWidth + style.ItemSpacing.x)));
        ImGui::InputTextWithHint("##geral-data-oitiva", "DD/MM/AAAA", module.general_data_oitiva.data(), module.general_data_oitiva.size(), ImGuiInputTextFlags_CallbackEdit, sto::text::date_mask_callback);
        ImGui::SameLine();
        if (ImGui::Button(kIconCalendar, {kCalendarButtonWidth, 0.0F})) {
            module.open_calendar(CalendarTarget::auto_transcricao_geral);
            ImGui::OpenPopup("calendar-popup");
        }
        render_calendar_popup(module);
        const float left_bottom = ImGui::GetCursorPosY();

        ImGui::SetCursorPos(cols.right_pos);
        ImGui::TextColored(kMuted, "Condição no documento");
        ImGui::SetCursorPos({cols.right_pos.x, ImGui::GetCursorPosY()});
        ImGui::SetNextItemWidth(cols.right_width);
        ImGui::InputTextWithHint("##geral-qualificacao", "testemunha, vítima, investigado...", module.general_qualificacao.data(), module.general_qualificacao.size());

        end_two_columns(cols, left_bottom);
    }
    ImGui::Spacing();

    ImGui::TextColored(kMuted, "Transcrição/resumo da oitiva");
    const float relato_height = std::max(
        120.0F,
        ImGui::GetContentRegionAvail().y - 46.0F - 36.0F - ImGui::GetTextLineHeightWithSpacing() * 4.0F - style.ItemSpacing.y * 8.0F
    );
    ImGui::InputTextMultiline(
        "##geral-relato", module.general_relato.data(), module.general_relato.size(), {-1.0F, relato_height},
        ImGuiInputTextFlags_WordWrap
    );
    ImGui::Spacing();
    if (editing_general_oitiva) sync_general_editor_to_selected(module);
    ImGui::EndDisabled();

    accent_line(ImGui::GetContentRegionAvail().x, kSilver);
    ImGui::Spacing();

    if (!module.validation_error.empty()) {
        wrapped_text(kError, module.validation_error);
        ImGui::Spacing();
    }

    if (colored_button(label(kIconPdf, "Gerar e abrir no navegador"), {-1.0F, 46.0F}, kGreen)) start_general_generation(module);
    if (module.duplicate_relato_confirmation) ImGui::OpenPopup("Relatos semelhantes detectados");
    if (ImGui::BeginPopupModal("Relatos semelhantes detectados", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4{1.00F, 0.76F, 0.03F, 1.00F}, "ATENÇÃO — Possível erro humano");
        ImGui::Spacing();
        ImGui::TextUnformatted("Foram encontrados relatos idênticos ou muito semelhantes:");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kError);
        ImGui::TextUnformatted(module.duplicate_relato_warning.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextColored(kSilver, "Verifique se o conteúdo foi colado corretamente em cada oitiva.");
        ImGui::Spacing();
        center_popup_buttons(160.0F + style.ItemSpacing.x + 130.0F);
        if (colored_button("Revisar oitivas", {160.0F, 36.0F}, kBlue)) {
            module.duplicate_relato_confirmation = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (colored_button(label(kIconPdf, "Gerar mesmo assim"), {130.0F, 36.0F}, kRed)) {
            module.duplicate_relato_confirmation = false;
            ImGui::CloseCurrentPopup();
            start_general_generation(module, true);
        }
        ImGui::EndPopup();
    }
    if (colored_button(label(kIconTrash, "Limpar conteúdo"), {-1.0F, 36.0F}, kRed)) {
        if (auto_transcricao_geral_has_content(module)) {
            ImGui::OpenPopup("Confirmar limpeza do conteúdo geral");
        } else {
            clear_auto_transcricao_geral_content(module);
        }
    }
    if (ImGui::BeginPopupModal("Confirmar limpeza do conteúdo geral", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Deseja realmente limpar o conteúdo do auto de transcrição geral?");
        ImGui::TextColored(kSilver, "Isso apaga os dados do procedimento e todas as oitivas da lista.");
        ImGui::Spacing();
        center_popup_buttons(130.0F + style.ItemSpacing.x + 110.0F);
        if (colored_button(label(kIconTrash, "Limpar"), {130.0F, 36.0F}, kRed)) {
            clear_auto_transcricao_geral_content(module);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", {110.0F, 36.0F})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    render_result(module);
    ImGui::EndChild();

    if (side_by_side) ImGui::SameLine();
    else ImGui::Spacing();

    ImGui::BeginChild("general-queue-right", {side_by_side ? right_width : 0.0F, 0.0F}, ImGuiChildFlags_Borders);
    const char* queue_collapse_icon = module.general_queue_collapsed ? kIconChevronLeft : kIconChevronRight;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 38.0F);
    if (ImGui::Button(queue_collapse_icon, {28.0F, 28.0F})) module.general_queue_collapsed = !module.general_queue_collapsed;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(module.general_queue_collapsed ? "Expandir lista de oitivas" : "Recolher lista de oitivas");

    if (!module.general_queue_collapsed) {
        ImGui::TextColored(kWhite, "Lista de oitivas (%zu)", module.general_oitivas.size());
        wrapped_text(kMuted, "A ordem romana é recalculada automaticamente pela data da oitiva.");
    }
    ImGui::Spacing();
    if (colored_button(
        module.general_queue_collapsed ? std::string(kIconPlus) + "##add-oitiva" : label(kIconPlus, "Adicionar oitiva"),
        {-1.0F, 38.0F}, kBlue
    )) {
        const auto snap = sto::modules::transcription::get_selected_session_snapshot();
        create_general_oitiva_draft(module);
        if (snap.has_session && !module.general_oitivas.empty()) {
            auto& draft = module.general_oitivas.back();
            draft.tipo_oitiva = std::max(0, snap.hearing_type - 1);
            draft.nome_parte = snap.party_name;
            draft.qualificacao = qualificacao_padrao(draft.tipo_oitiva);
            draft.data_oitiva = snap.date_oitiva;
            load_general_oitiva_to_editor(module, module.general_oitivas.size() - 1);
        }
    }
    if (module.general_queue_collapsed && ImGui::IsItemHovered()) ImGui::SetTooltip("Adicionar oitiva");
    ImGui::Spacing();
    const float list_height = std::max(220.0F, ImGui::GetContentRegionAvail().y);
    render_general_oitivas_list(module, {0.0F, list_height});
    ImGui::EndChild();
}
}

void render_document_settings() {
    State& module = state();
    constexpr const char* kIconCancel = "\xef\x80\x8d"; // fa-xmark

    // --- Logo preview (auto-resize) ---
    static std::string last_logo_path = "\x01";
    if (module.tmpl_logo_path != last_logo_path) {
        last_logo_path = module.tmpl_logo_path;
        const auto path_to_load = module.tmpl_logo_path.empty()
            ? (sto::files::executable_directory() / "assets" / "documents" / "logo_pcsc.png").string()
            : module.tmpl_logo_path;
        sto::ui::request_doc_logo_reload(path_to_load);
    }

    const bool has_custom_logo = !module.tmpl_logo_path.empty()
        && std::filesystem::exists(module.tmpl_logo_path);
    const ImTextureID logo_tex = sto::ui::doc_logo_texture();
    const float       logo_w   = static_cast<float>(sto::ui::doc_logo_width());
    const float       logo_h   = static_cast<float>(sto::ui::doc_logo_height());

    constexpr float preview_max_h = 96.0F;
    ImGui::BeginChild("##logo-box", {-1.0F, 0.0F}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(kMuted, has_custom_logo ? "Logo personalizada" : "Logo padrão (PCSC)");
    if (logo_tex != ImTextureID_Invalid && logo_w > 0.0F && logo_h > 0.0F) {
        const float scale = std::min(1.0F, preview_max_h / logo_h);
        ImGui::Image(ImTextureRef(logo_tex), {logo_w * scale, logo_h * scale});
    }
    ImGui::Spacing();
    if (colored_button(label(kIconFolder, "Alterar logo"), {190.0F, 34.0F}, kBlue)) {
        const auto files = sto::platform::select_files(L"Selecione a logo (PNG)", {{L"Imagens PNG", L"*.png"}});
        if (!files.empty()) {
            const auto dest = sto::files::executable_directory() / "data" / "logo_custom.png";
            std::error_code ec;
            std::filesystem::create_directories(dest.parent_path(), ec);
            std::filesystem::copy_file(files.front(), dest, std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) { module.tmpl_logo_path = dest.string(); last_logo_path = "\x01"; }
        }
    }
    if (has_custom_logo) {
        ImGui::SameLine();
        if (colored_button("Restaurar padrão", {150.0F, 34.0F}, kRed, kWhite)) {
            module.tmpl_logo_path.clear(); last_logo_path = "\x01";
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();

    // --- Seletor + botão Editar ---
    constexpr const char* kDocTypes[] = {"Auto de Transcrição", "Auto de Transcrição Geral", "Auto de Transcrição de Áudios"};
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::Combo("##doc-type-sel", &module.tmpl_selector, kDocTypes, 3);
    ImGui::Spacing();
    static bool open_editor = false;
    if (colored_button(label(kIconPdf, "Editar textos do documento"), {-1.0F, 36.0F}, kBlue))
        open_editor = true;

    // --- Popup de edição ---
    if (open_editor) { ImGui::OpenPopup("##doc-text-editor"); open_editor = false; }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
    ImGui::SetNextWindowSize({vp->WorkSize.x * 0.88F, vp->WorkSize.y * 0.88F}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##doc-text-editor", nullptr, ImGuiWindowFlags_None)) {
        const bool is_geral = module.tmpl_selector == 1;
        const bool is_audio = module.tmpl_selector == 2;
        const char* type = is_audio ? "Auto de Transcrição de Áudios"
            : is_geral ? "Auto de Transcrição Geral"
            : "Auto de Transcrição";

        if (sto::ui::section_font()) ImGui::PushFont(sto::ui::section_font());
        ImGui::TextColored(kWhite, "%s", type);
        if (sto::ui::section_font()) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        constexpr float button_h   = 44.0F;
        constexpr float field_h    = 80.0F;
        constexpr float intro_actual = 160.0F;

        // Cabeçalho
        ImGui::TextColored(kMuted, "Cabeçalho (linhas exibidas abaixo da logo, uma por linha)");
        ImGui::InputTextMultiline("##hdr", module.tmpl_header_text.data(), module.tmpl_header_text.size(),
            {-1.0F, field_h});
        ImGui::Spacing();

        if (is_audio) {
            ImGui::TextColored(kMuted, "Título do documento");
            ImGui::InputText("##audio-title", module.tmpl_audio_title.data(), module.tmpl_audio_title.size());
            ImGui::Spacing();
        }

        // Parágrafo introdutório
        auto& intro_buf = is_audio ? module.tmpl_audio_intro : is_geral ? module.tmpl_intro_geral : module.tmpl_intro_auto;
        ImGui::TextColored(kMuted, "Parágrafo introdutório");
        ImGui::InputTextMultiline("##intro", intro_buf.data(), intro_buf.size(),
            {-1.0F, intro_actual}, ImGuiInputTextFlags_WordWrap);
        ImGui::Spacing();

        // Título de cada oitiva (apenas no Geral)
        if (is_geral) {
            ImGui::TextColored(kMuted, "Título de cada oitiva (o numeral romano é adicionado automaticamente)");
            ImGui::InputTextMultiline("##oitiva-tmpl", module.tmpl_oitiva_geral.data(), module.tmpl_oitiva_geral.size(),
                {-1.0F, field_h}, ImGuiInputTextFlags_WordWrap);
            ImGui::Spacing();
        }
        if (is_audio) {
            ImGui::TextColored(kMuted, "Título de cada arquivo de áudio");
            ImGui::InputTextMultiline("##audio-entry-title", module.tmpl_audio_entry_title.data(), module.tmpl_audio_entry_title.size(),
                {-1.0F, field_h}, ImGuiInputTextFlags_WordWrap);
            ImGui::Spacing();
        }

        // Parágrafo de encerramento
        auto& closing_buf = is_audio ? module.tmpl_audio_closing : is_geral ? module.tmpl_closing_geral : module.tmpl_closing_auto;
        ImGui::TextColored(kMuted, "Parágrafo de encerramento");
        ImGui::InputTextMultiline("##closing", closing_buf.data(), closing_buf.size(),
            {-1.0F, field_h}, ImGuiInputTextFlags_WordWrap);
        ImGui::Spacing();

        if (is_audio) {
            ImGui::TextColored(kMuted, "Nota de rodapé: Whisper");
            ImGui::InputTextMultiline("##audio-note-whisper", module.tmpl_audio_note_whisper.data(), module.tmpl_audio_note_whisper.size(),
                {-1.0F, field_h}, ImGuiInputTextFlags_WordWrap);
            ImGui::Spacing();
            ImGui::TextColored(kMuted, "Nota de rodapé: modelo");
            ImGui::InputTextMultiline("##audio-note-model", module.tmpl_audio_note_model.data(), module.tmpl_audio_note_model.size(),
                {-1.0F, field_h}, ImGuiInputTextFlags_WordWrap);
            ImGui::Spacing();
        }

        // Rodapé (HTML direto)
        ImGui::TextColored(kMuted, "Rodapé");
        ImGui::SameLine();
        ImGui::TextColored(kSubtle, "(HTML — use <b>texto</b> para negrito, <p> para parágrafo)");
        ImGui::InputTextMultiline("##ftr", module.tmpl_footer_text.data(), module.tmpl_footer_text.size(),
            {-1.0F, field_h}, ImGuiInputTextFlags_WordWrap);
        ImGui::Spacing();

        // Legenda de marcadores
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.05F, 0.055F, 0.06F, 1.0F});
        const float legend_h = is_audio ? 190.0F : is_geral ? 150.0F : 120.0F;
        ImGui::BeginChild("##legend", {-1.0F, legend_h}, ImGuiChildFlags_Borders);
        ImGui::TextColored(kMuted, "Marcadores disponíveis no parágrafo introdutório:");
        if (is_audio) {
            ImGui::TextColored(kMuted, "Parágrafo introdutório:");
            ImGui::TextColored(kSilver, "$$DATA_EXTENSO$$");    ImGui::SameLine(220); ImGui::TextColored(kMuted, "Extenso da data de hoje");
            ImGui::TextColored(kSilver, "$$NOME_DELEGADO$$");   ImGui::SameLine(220); ImGui::TextColored(kMuted, "Nome do delegado");
            ImGui::TextColored(kSilver, "$$PROCEDIMENTO$$");    ImGui::SameLine(220); ImGui::TextColored(kMuted, "Ex: Inquérito Policial nº 264.24.00178");
            ImGui::TextColored(kSilver, "$$TIPO_PROCEDIMENTO$$"); ImGui::SameLine(220); ImGui::TextColored(kMuted, "Tipo do procedimento");
            ImGui::TextColored(kSilver, "$$NUMERO_PROCEDIMENTO$$"); ImGui::SameLine(220); ImGui::TextColored(kMuted, "Número informado");
            ImGui::TextColored(kSilver, "$$NOTA_WHISPER$$");    ImGui::SameLine(220); ImGui::TextColored(kMuted, "Chamada da nota sobre Whisper");
            ImGui::TextColored(kSilver, "$$NOTA_MODELO$$");     ImGui::SameLine(220); ImGui::TextColored(kMuted, "Chamada da nota sobre o modelo");
            ImGui::Separator();
            ImGui::TextColored(kMuted, "Título de cada arquivo:");
            ImGui::TextColored(kSilver, "$$NUMERAL$$");         ImGui::SameLine(220); ImGui::TextColored(kMuted, "Numeral romano do item");
            ImGui::TextColored(kSilver, "$$NOME_ARQUIVO$$");    ImGui::SameLine(220); ImGui::TextColored(kMuted, "Nome exibido na fila");
            ImGui::TextColored(kSilver, "$$DESCRICAO$$");       ImGui::SameLine(220); ImGui::TextColored(kMuted, "Descrição opcional");
            ImGui::TextColored(kSilver, "$$DESCRICAO_PARENTESES$$"); ImGui::SameLine(220); ImGui::TextColored(kMuted, "Descrição com parênteses, se existir");
        } else if (is_geral) {
            ImGui::TextColored(kMuted, "Parágrafo introdutório:");
            ImGui::TextColored(kSilver, "$$DATA_EXTENSO$$");   ImGui::SameLine(180); ImGui::TextColored(kMuted, "Extenso da data de hoje");
            ImGui::TextColored(kSilver, "$$NOME_DELEGADO$$"); ImGui::SameLine(180); ImGui::TextColored(kMuted, "Nome do delegado");
            ImGui::TextColored(kSilver, "$$PROCEDIMENTO$$");  ImGui::SameLine(180); ImGui::TextColored(kMuted, "Ex: Inquérito Policial nº 264.24.00178");
            ImGui::Separator();
            ImGui::TextColored(kMuted, "Título de cada oitiva:");
            ImGui::TextColored(kSilver, "$$NOME_PARTE$$");     ImGui::SameLine(180); ImGui::TextColored(kMuted, "Nome da parte ouvida");
            ImGui::TextColored(kSilver, "$$QUALIFICACAO$$");   ImGui::SameLine(180); ImGui::TextColored(kMuted, "Ex: Testemunha, Declarante...");
            ImGui::TextColored(kSilver, "$$DATA_OITIVA$$");    ImGui::SameLine(180); ImGui::TextColored(kMuted, "Data da oitiva");
        } else {
            ImGui::TextColored(kSilver, "$$DATA_EXTENSO$$");    ImGui::SameLine(180); ImGui::TextColored(kMuted, "Extenso da data de hoje");
            ImGui::TextColored(kSilver, "$$NOME_DELEGADO$$");   ImGui::SameLine(180); ImGui::TextColored(kMuted, "Nome do delegado");
            ImGui::TextColored(kSilver, "$$TIPO_OITIVA$$");     ImGui::SameLine(180); ImGui::TextColored(kMuted, "Ex: interrogatório, depoimento...");
            ImGui::TextColored(kSilver, "$$NOME_PARTE$$");      ImGui::SameLine(180); ImGui::TextColored(kMuted, "Nome da parte ouvida");
            ImGui::TextColored(kSilver, "$$DATA_OITIVA$$");     ImGui::SameLine(180); ImGui::TextColored(kMuted, "Data da oitiva (DD/MM/AAAA)");
            ImGui::TextColored(kSilver, "$$PROCEDIMENTO$$");    ImGui::SameLine(180); ImGui::TextColored(kMuted, "Ex: Inquérito Policial nº 264.24.00178");
            ImGui::TextColored(kSilver, "$$LOCAL_ESPECIAL$$");  ImGui::SameLine(180); ImGui::TextColored(kMuted, "Local (Depoimento Especial)");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Botões
        ImGui::Spacing();
        static std::string doc_save_note;
        static bool doc_save_ok = true;
        const float total_w = ImGui::GetContentRegionAvail().x;
        const float half_w  = (total_w - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
        if (colored_button(label(kIconSave, "Salvar"), {half_w, button_h}, kGreen)) {
            if (module.database_ready) {
                std::string error;
                const sto::database::DocumentTemplate td{
                    module.tmpl_header_text.data(), module.tmpl_footer_text.data(),
                    module.tmpl_intro_auto.data(),  module.tmpl_intro_geral.data(),
                    module.tmpl_closing_auto.data(),module.tmpl_closing_geral.data(),
                    module.tmpl_oitiva_geral.data(),
                    module.tmpl_audio_title.data(), module.tmpl_audio_intro.data(),
                    module.tmpl_audio_entry_title.data(), module.tmpl_audio_closing.data(),
                    module.tmpl_audio_note_whisper.data(), module.tmpl_audio_note_model.data(),
                    module.tmpl_logo_path};
                doc_save_ok   = module.repository.save_template(td, error);
                doc_save_note = doc_save_ok ? "" : "Erro: " + error;
                if (doc_save_ok) ImGui::CloseCurrentPopup();
            } else {
                doc_save_ok   = false;
                doc_save_note = "Banco de dados não disponível.";
            }
        }
        if (!doc_save_note.empty()) ImGui::TextColored(kError, "%s", doc_save_note.c_str());
        ImGui::SameLine();
        if (colored_button(label(kIconCancel, "Cancelar"), {half_w, button_h}, kRed)) {
            doc_save_note.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void render() {
    State& module = state();
    constexpr float horizontal_margin = 10.0F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + horizontal_margin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0F, 8.0F});
    ImGui::BeginChild(
        "documents-module-frame",
        {ImGui::GetContentRegionAvail().x - horizontal_margin, 0.0F},
        false
    );
    ImGui::TextColored(kSilver, "MÓDULOS  /  GERAÇÃO DE DOCUMENTOS");
    ImGui::Spacing();
    switch (module.document) {
    case Document::dashboard: render_dashboard(module); break;
    case Document::auto_transcricao: render_auto_transcricao(module); break;
    case Document::auto_transcricao_geral: render_auto_transcricao_geral(module); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}
}
