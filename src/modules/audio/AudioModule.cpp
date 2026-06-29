#include "modules/audio/AudioModule.hpp"

#include "core/database/AudioRepository.hpp"
#include "core/database/DocumentsRepository.hpp"
#include "core/documents/HtmlDocument.hpp"
#include "core/files/FileUtils.hpp"
#include "core/platform/FileDialogs.hpp"
#include "core/platform/ProcessRunner.hpp"
#include "core/text/PortugueseDate.hpp"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ui/Theme.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <shellapi.h>

namespace document_html = sto::documents;

namespace sto::modules::audio {
namespace {

// ---- Constants ----

constexpr const char* kIconPlay      = "\xef\x81\x8b";
constexpr const char* kIconPause     = "\xef\x81\x8c";
constexpr const char* kIconCancel    = "\xef\x81\x8d";
constexpr const char* kIconTrash     = "\xef\x87\xb8";
constexpr const char* kIconFolder    = "\xef\x81\xbb";
constexpr const char* kIconCheck     = "\xef\x80\x8c";
constexpr const char* kIconClock     = "\xef\x80\x97";
constexpr const char* kIconGear      = "\xef\x80\x93";
constexpr const char* kIconPdf       = "\xef\x87\x81";
constexpr const char* kIconChevronL  = "\xef\x81\x93";
constexpr const char* kIconChevronR  = "\xef\x81\x94";

constexpr ImVec4 kWhite   = {0.94F, 0.95F, 0.96F, 1.00F};
constexpr ImVec4 kSilver  = {0.70F, 0.73F, 0.76F, 1.00F};
constexpr ImVec4 kMuted   = {0.52F, 0.55F, 0.58F, 1.00F};
constexpr ImVec4 kSubtle  = {0.36F, 0.39F, 0.42F, 1.00F};
constexpr ImVec4 kSuccess = {0.20F, 1.00F, 0.36F, 1.00F};
constexpr ImVec4 kError   = {1.00F, 0.22F, 0.22F, 1.00F};
constexpr ImVec4 kBlue    = {0.00F, 0.48F, 1.00F, 1.00F};
constexpr ImVec4 kGreen   = {0.16F, 0.65F, 0.27F, 1.00F};
constexpr ImVec4 kRed     = {0.86F, 0.21F, 0.27F, 1.00F};
constexpr ImVec4 kYellow  = {1.00F, 0.76F, 0.03F, 1.00F};
constexpr ImVec4 kDark    = {0.05F, 0.06F, 0.07F, 1.00F};

constexpr std::array kProcedimentoTypes = {
    "Boletim de Ocorrência",
    "Inquérito Policial",
    "Termo Circunstanciado de Ocorrência",
    "Auto de Prisão em Flagrante Delito",
    "Outros",
};
constexpr std::array kProcedimentoSiglas = {
    "BO", "IP", "TCO", "APF", "OUT",
};

// ---- Helpers ----

enum class Status { idle, queued, running, paused, completed, failed };
enum class NoteKind { success, error };

std::filesystem::path runtime_path(const wchar_t* rel) {
    return sto::files::executable_directory() / rel;
}

std::filesystem::path db_path() {
    return sto::files::executable_directory() / "data" / "sto.db";
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(n - 1));
    return out;
}

std::string detect_processor_model() {
    std::array<wchar_t, 256> value{};
    DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString",
        RRF_RT_REG_SZ,
        nullptr,
        value.data(),
        &size) == ERROR_SUCCESS) {
        return narrow(value.data());
    }
    return "unknown-processor";
}

double probe_duration(const std::filesystem::path& input) {
    std::atomic_bool cancel = false;
    const auto result = sto::platform::run_process({
        runtime_path(L"runtime/ffmpeg/ffprobe.exe").wstring(),
        L"-v", L"error", L"-show_entries", L"format=duration",
        L"-of", L"default=nw=1:nk=1", input.wstring()
    }, cancel);
    try { return result.exit_code == 0 ? std::stod(result.output) : 0.0; }
    catch (...) { return 0.0; }
}

std::string roman_numeral(int n) {
    constexpr struct { int v; const char* r; } table[] = {
        {1000,"M"},{900,"CM"},{500,"D"},{400,"CD"},{100,"C"},{90,"XC"},
        {50,"L"},{40,"XL"},{10,"X"},{9,"IX"},{5,"V"},{4,"IV"},{1,"I"}
    };
    std::string result;
    for (const auto& [v, r] : table) { while (n >= v) { result += r; n -= v; } }
    return result;
}

std::string join_transcript_lines(const std::string& raw) {
    // Join whisper output lines into flowing paragraphs
    std::string out;
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        // strip leading/trailing whitespace
        auto start = line.find_first_not_of(" \t\r");
        auto end   = line.find_last_not_of(" \t\r");
        if (start == std::string::npos) continue;
        line = line.substr(start, end - start + 1);
        if (line.empty()) continue;
        if (!out.empty()) out += ' ';
        out += line;
    }
    return out;
}

bool colored_button(const char* label, const ImVec2& size, const ImVec4& col,
                    const ImVec4& text_col = kWhite) {
    ImGui::PushStyleColor(ImGuiCol_Button,        col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {std::min(col.x+0.1F,1.F),std::min(col.y+0.1F,1.F),std::min(col.z+0.1F,1.F),1.F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {col.x*0.78F,col.y*0.78F,col.z*0.78F,1.F});
    ImGui::PushStyleColor(ImGuiCol_Text,          text_col);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}
bool colored_button(const std::string& label, const ImVec2& size, const ImVec4& col,
                    const ImVec4& text_col = kWhite) {
    return colored_button(label.c_str(), size, col, text_col);
}

std::string btn(const char* icon, const char* text) {
    return std::string(icon) + "  " + text;
}

// ---- Session ----

struct Session {
    long long id = 0;
    std::string              display_name;
    std::string              description;
    std::filesystem::path    input;
    std::string              transcript;
    Status                   status = Status::idle;
    int                      queue_order = 0;

    std::atomic<float>     progress{0.0F};        // combined 0-1 (conv + whisper)
    std::atomic<float>     whisper_progress{0.0F}; // whisper-only 0-1 (for time estimate)
    std::atomic_bool       transcribing{false};
    std::atomic<double>    estimated_seconds{0.0};
    std::atomic<double>    projected_total_seconds{0.0};
    std::atomic<long long> transcription_started_ms{0};
    std::string message;
    std::mutex  msg_mutex;

    bool busy() const { return status == Status::running; }

    void set_message(std::string msg) {
        std::scoped_lock lk(msg_mutex);
        message = std::move(msg);
    }
    std::string get_message() {
        std::scoped_lock lk(msg_mutex);
        return message;
    }
};

// ---- Helpers ----

std::string format_duration(double seconds) {
    const int r = std::max(0, static_cast<int>(std::ceil(seconds)));
    const int h = r / 3600, m = (r % 3600) / 60, s = r % 60;
    if (h > 0) return std::format("{}h {:02d}min {:02d}s", h, m, s);
    if (m > 0) return std::format("{}min {:02d}s", m, s);
    return std::format("{}s", s);
}

// ---- State ----

struct State {
    sto::database::AudioRepository repository;
    bool initialized   = false;
    bool database_ready = false;

    std::vector<std::shared_ptr<Session>> sessions;
    std::shared_ptr<Session> selected;
    std::vector<long long>   queue_selection;
    long long                selection_anchor_id = 0;

    // Queue worker
    std::jthread            worker;
    std::atomic_bool        cancel_requested = false;
    std::atomic_bool        pause_after_cancel = false;
    bool                    pause_running_confirmation = false;
    bool                    requeue_overwrite_confirmation = false;
    bool                    bulk_delete_confirmation = false;
    bool                    retranscribe_confirmation = false;
    bool                    clear_confirmation = false;

    // Document generation fields
    int                      tipo_procedimento = 0;
    std::array<char, 64>     numero_procedimento{};
    std::array<char, 256>    nome_delegado{};
    std::array<char, 256>    nome_responsavel{};
    std::array<char, 128>    cargo_responsavel{};
    bool                     show_gen_confirm = false;

    // Sidebar
    bool sidebar_collapsed = false;

    // Inline note
    std::string  note_text;
    NoteKind     note_kind = NoteKind::success;
    std::chrono::steady_clock::time_point note_time;

    void set_single_selection(const std::shared_ptr<Session>& s) {
        selected = s;
        queue_selection.clear();
        if (s) { queue_selection.push_back(s->id); selection_anchor_id = s->id; }
        else     selection_anchor_id = 0;
    }
    bool is_selected(long long id) const {
        return std::find(queue_selection.begin(), queue_selection.end(), id) != queue_selection.end();
    }
    void handle_click(long long id, bool ctrl, bool shift) {
        auto it = std::find_if(sessions.begin(), sessions.end(),
            [id](const auto& s){ return s->id == id; });
        if (it == sessions.end()) return;
        selected = *it;
        if (shift && selection_anchor_id != 0) {
            // Range select between anchor and target
            auto anchor_it = std::find_if(sessions.begin(), sessions.end(),
                [this](const auto& s){ return s->id == selection_anchor_id; });
            if (anchor_it != sessions.end()) {
                const auto [lo, hi] = std::minmax(anchor_it, it);
                queue_selection.clear();
                for (auto cur = lo; cur <= hi; ++cur) queue_selection.push_back((*cur)->id);
                return;
            }
        }
        if (ctrl) {
            auto sel_it = std::find(queue_selection.begin(), queue_selection.end(), id);
            if (sel_it != queue_selection.end()) queue_selection.erase(sel_it);
            else queue_selection.push_back(id);
            selection_anchor_id = id;
        } else {
            queue_selection = {id};
            selection_anchor_id = id;
        }
    }
    void set_note(std::string text, NoteKind kind = NoteKind::success) {
        note_text = std::move(text);
        note_kind = kind;
        note_time = std::chrono::steady_clock::now();
    }

    void initialize() {
        if (initialized) return;
        initialized = true;
        std::string error;
        database_ready = repository.initialize(db_path(), error);
        if (database_ready) {
            const auto stored = repository.load_sessions(error);
            for (const auto& s : stored) {
                auto session = std::make_shared<Session>();
                session->id = s.id;
                session->input = s.input_path;
                session->queue_order = s.queue_order;
                session->display_name = s.display_name;
                session->description = s.description;
                session->transcript = s.transcript;
                // Resume paused sessions as idle
                if (s.status == "queued" || s.status == "running" || s.status == "paused")
                    session->status = Status::paused;
                else if (s.status == "completed")
                    session->status = Status::completed;
                else if (s.status == "failed")
                    session->status = Status::failed;
                else
                    session->status = Status::idle;
                sessions.push_back(session);
            }
            if (!sessions.empty()) selected = sessions.front();

            // Load delegado name from shared DocumentPreferences
            sto::database::DocumentsRepository doc_repo;
            if (doc_repo.initialize(db_path(), error)) {
                const auto prefs = doc_repo.load(error);
                std::snprintf(nome_delegado.data(), nome_delegado.size(), "%s", prefs.nome_delegado.c_str());
                std::snprintf(nome_responsavel.data(), nome_responsavel.size(), "%s", prefs.nome_responsavel.c_str());
                std::snprintf(cargo_responsavel.data(), cargo_responsavel.size(), "%s", prefs.cargo_responsavel.c_str());
            }
        }
    }

    void save_session(const std::shared_ptr<Session>& s) {
        if (!database_ready || !s) return;
        sto::database::AudioSession stored;
        stored.id           = s->id;
        stored.display_name = s->display_name;
        stored.description  = s->description;
        stored.input_path   = narrow(s->input.wstring());
        stored.transcript   = s->transcript;
        stored.queue_order  = s->queue_order;
        switch (s->status) {
        case Status::completed: stored.status = "completed"; break;
        case Status::failed:    stored.status = "failed";    break;
        case Status::queued:    stored.status = "queued";    break;
        case Status::paused:    stored.status = "paused";    break;
        default:                stored.status = "idle";      break;
        }
        std::string err;
        repository.save_session(stored, err);
    }

    std::shared_ptr<Session> find_session(long long id) {
        const auto it = std::find_if(sessions.begin(), sessions.end(),
            [id](const auto& s) { return s && s->id == id; });
        return it == sessions.end() ? nullptr : *it;
    }

    void set_selection_status(Status new_status) {
        bool changed = false;
        for (long long id : queue_selection) {
            auto s = find_session(id);
            if (!s || s->busy() || s->input.empty()) continue;
            s->status = new_status;
            s->set_message(new_status == Status::queued
                ? "Transcrição adicionada à fila."
                : "Transcrição pausada. Clique em Transcrever para reativar.");
            save_session(s);
            changed = true;
        }
        if (changed) set_note(new_status == Status::queued ? "Itens adicionados à fila." : "Itens pausados.");
    }

    void request_pause_selection() {
        const bool has_running = std::any_of(queue_selection.begin(), queue_selection.end(),
            [this](long long id) {
                auto s = find_session(id);
                return s && s->busy();
            });
        if (has_running) {
            pause_running_confirmation = true;
            return;
        }
        set_selection_status(Status::paused);
    }

    void request_queue_selection() {
        const bool overwrites_transcript = std::any_of(queue_selection.begin(), queue_selection.end(),
            [this](long long id) {
                auto s = find_session(id);
                return s && !s->busy() && !s->input.empty() && !s->transcript.empty();
            });
        if (overwrites_transcript) {
            requeue_overwrite_confirmation = true;
            return;
        }
        set_selection_status(Status::queued);
    }

    void confirm_pause_running() {
        pause_after_cancel = true;
        cancel_requested = true;
        set_selection_status(Status::paused);
        pause_running_confirmation = false;
    }

    void remove_queue_selection() {
        bool removed = false;
        for (long long id : queue_selection) {
            auto s = find_session(id);
            if (!s || s->busy()) continue;
            std::string err;
            if (!repository.delete_session(id, err)) {
                set_note("Não foi possível remover item: " + err, NoteKind::error);
                continue;
            }
            sessions.erase(std::remove(sessions.begin(), sessions.end(), s), sessions.end());
            removed = true;
        }
        const bool selection_lost = !selected || std::find(sessions.begin(), sessions.end(), selected) == sessions.end();
        set_single_selection(selection_lost ? (sessions.empty() ? nullptr : sessions.front()) : selected);
        if (removed) set_note("Itens removidos com sucesso.");
    }
};

State& state() {
    static State instance;
    instance.initialize();
    return instance;
}

// ---- Transcription ----

void transcribe_session(State& module, std::shared_ptr<Session> session) {
    const auto selected_input = session->input;
    session->status = Status::running;
    session->progress = 0.0F;
    session->whisper_progress = 0.0F;
    session->estimated_seconds = 0.0;
    session->projected_total_seconds = 0.0;
    session->transcribing = false;
    session->transcript.clear();
    module.save_session(session);

    const auto tmp = std::filesystem::temp_directory_path() / L"sto_audio_transcription";
    std::error_code ec;
    std::filesystem::create_directories(tmp, ec);
    if (ec) {
        session->status = Status::failed;
        session->set_message("Não foi possível criar diretório temporário.");
        module.save_session(session);
        return;
    }

    const auto stem = sto::files::sanitize_stem(selected_input.stem().wstring());
    const auto wav  = sto::files::unique_path(tmp / (stem + L".wav"));
    const auto txt  = std::filesystem::path(wav.wstring() + L".txt");
    const double media_duration = probe_duration(selected_input);

    const auto cleanup = [&]{ std::filesystem::remove(wav, ec); std::filesystem::remove(txt, ec); };
    const auto fail = [&](std::string msg) {
        session->transcribing = false;
        session->status = Status::failed;
        session->set_message(std::move(msg));
        module.save_session(session);
        cleanup();
    };
    const auto pause_or_cancel = [&] {
        session->transcribing = false;
        session->status = Status::paused;
        session->set_message(module.pause_after_cancel.exchange(false)
            ? "Transcrição pausada. Clique em Transcrever para reativar."
            : "Transcrição pausada.");
        module.save_session(session);
        cleanup();
    };

    // Convert to 16kHz mono WAV
    session->set_message("Convertendo áudio...");
    session->progress = 0.05F;
    const auto conv = sto::platform::run_process({
        runtime_path(L"runtime/ffmpeg/ffmpeg.exe").wstring(),
        L"-hide_banner", L"-nostats", L"-y", L"-i", selected_input.wstring(),
        L"-vn", L"-ar", L"16000", L"-ac", L"1", L"-c:a", L"pcm_s16le", wav.wstring()
    }, module.cancel_requested);
    if (conv.cancelled) {
        session->status = Status::paused;
        session->set_message("Transcrição pausada.");
        module.save_session(session);
        cleanup();
        return;
    }
    if (conv.exit_code != 0 || !std::filesystem::exists(wav)) {
        fail("FFmpeg não conseguiu preparar o áudio.");
        return;
    }

    // Run Whisper
    session->set_message("Transcrevendo com Whisper...");
    const std::string processor_model = detect_processor_model();
    std::string estimate_error;
    session->estimated_seconds = module.repository.estimate_seconds(processor_model, media_duration, estimate_error);
    session->projected_total_seconds = session->estimated_seconds.load();
    session->progress = 0.0F;
    session->whisper_progress = 0.0F;
    session->transcribing = true;
    session->transcription_started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto transcription_started = std::chrono::steady_clock::now();
    const auto whisper_dir = runtime_path(L"runtime/whisper");
    const auto whisper = sto::platform::run_process({
        (whisper_dir / L"whisper-cli.exe").wstring(),
        L"-m", (whisper_dir / L"ggml-large-v3-turbo.bin").wstring(),
        L"-f", wav.wstring(), L"-l", L"pt", L"-otxt", L"-np", L"-pp",
        L"-t", std::to_wstring(std::max(1U, std::thread::hardware_concurrency())), L"-p", L"1"
    }, module.cancel_requested, [&session](const std::string& line) {
        const auto pct = line.find('%');
        if (pct == std::string::npos) return;
        try {
            const auto start = line.find_last_not_of("0123456789", pct == 0 ? 0 : pct - 1);
            const float wp = std::clamp(
                std::stof(line.substr(start == std::string::npos ? 0 : start + 1)) / 100.0F,
                0.0F, 1.0F);
            session->whisper_progress = wp;
            session->progress = wp;
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const double elapsed = static_cast<double>(now - session->transcription_started_ms.load()) / 1000.0;
            if (wp >= 0.01F && wp < 1.0F && elapsed > 0.0) {
                const double implied_total = elapsed / static_cast<double>(wp);
                const double historical_total = session->estimated_seconds.load();
                const double anchor_weight = std::clamp(static_cast<double>(wp), 0.20, 0.85);
                session->projected_total_seconds = historical_total > 0.0
                    ? (historical_total * (1.0 - anchor_weight)) + (implied_total * anchor_weight)
                    : implied_total;
            }
        } catch (...) {}
    }, whisper_dir.wstring());

    session->transcribing = false;
    if (whisper.cancelled) {
        session->status = Status::paused;
        session->set_message("Transcrição pausada.");
        module.save_session(session);
        cleanup();
        return;
    }
    if (!whisper.started) {
        fail("Whisper não pôde ser iniciado. Verifique runtime/whisper/whisper-cli.exe.");
        return;
    }
    if (whisper.exit_code != 0 || !std::filesystem::exists(txt)) {
        fail("Whisper não gerou o arquivo de saída. Verifique se o modelo está instalado.");
        return;
    }

    // Read result
    std::ifstream f(txt);
    std::string raw((std::istreambuf_iterator<char>(f)), {});
    const std::string joined = join_transcript_lines(raw);
    session->transcript = joined;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - transcription_started).count();
    if (media_duration > 0.0 && elapsed > 0.0) {
        std::string history_error;
        module.repository.record_history(processor_model, media_duration, elapsed, history_error);
    }
    session->status = Status::completed;
    session->progress = 1.0F;
    session->set_message("Concluído.");
    module.save_session(session);
    cleanup();
}

void start_queue_worker(State& module) {
    // Se alguma sessão está ATIVAMENTE rodando, não inicia outra
    for (const auto& s : module.sessions) {
        if (s->status == Status::running) return;
    }

    // Join do thread anterior que já terminou (libera recurso)
    if (module.worker.joinable()) {
        module.worker.request_stop();
        module.worker.join();
    }

    // Próxima sessão na fila
    std::shared_ptr<Session> target;
    for (auto& s : module.sessions) {
        if (s->status == Status::queued) { target = s; break; }
    }
    if (!target) return;

    target->status = Status::running;
    module.cancel_requested = false;
    module.worker = std::jthread([&module, target](std::stop_token) {
        transcribe_session(module, target);
        // Sem chamada recursiva — o render() chama start_queue_worker a cada frame
    });
}

// ---- Document generation ----

std::string audio_css() {
    return R"HTML(
    .audio-entry { margin-top: 4.5mm; }
    .audio-title { font-size: 11pt; line-height: 12pt; font-weight: 700; break-after: avoid; page-break-after: avoid; }
    .transcricao-box { margin-top: 2mm; border: 1px solid #000; padding: 1mm 1.2mm; font-size: 11pt; line-height: 14pt; }
    .transcricao-box p { text-align: justify; margin-bottom: 2mm; }
    .transcricao-box p:last-child { margin-bottom: 0; }
    .closing { margin-top: 7.5mm; text-align: justify; text-indent: 15mm; }
    .audio-footnote {
        float: footnote;
        footnote-policy: line;
        font-size: 8.5pt;
        line-height: 9.15pt;
        text-align: justify;
    }
    .audio-footnote::footnote-call {
        font-size: 6.8pt;
        vertical-align: super;
        line-height: 0;
    }
    .audio-footnote::footnote-marker {
        font-size: 6.2pt;
        line-height: 0;
    }
    .pagedjs_footnote_content:not(.pagedjs_footnote_empty) {
        font-size: 8.5pt;
        line-height: 9.15pt;
        text-align: justify;
    }
    .pagedjs_footnote_inner_content,
    .pagedjs_footnote_inner_content [data-note="footnote"],
    .pagedjs_footnote_inner_content [data-footnote-marker] {
        text-align: justify;
        text-align-last: left;
    }
    .pagedjs_footnote_content:not(.pagedjs_footnote_empty)::before {
        content: "";
        display: block;
        width: 106pt;
        border-top: 0.75pt solid #000;
        margin-bottom: 2mm;
    }
    .signature { margin-top: 12mm; text-align: center; font-size: 11pt; line-height: 12pt; break-inside: avoid; page-break-inside: avoid; }
    .signature .name { font-weight: 700; }
)HTML";
}

std::string replace_all(std::string text, const std::string& marker, const std::string& value) {
    std::size_t pos = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos) {
        text.replace(pos, marker.size(), value);
        pos += value.size();
    }
    return text;
}

std::string footnote_span(const std::string& note) {
    if (note.empty()) return {};
    return "<span class=\"audio-footnote\">" + document_html::escape_html(note) + "</span>";
}

std::string render_audio_intro_template(
    const std::string& tmpl,
    const std::string& data_extenso,
    const std::string& delegado,
    const std::string& procedimento,
    const std::string& tipo_procedimento,
    const std::string& numero_procedimento,
    const std::string& note_whisper,
    const std::string& note_model) {
    std::string html = document_html::escape_html(tmpl);
    html = replace_all(std::move(html), "$$DATA_EXTENSO$$", document_html::escape_html(data_extenso));
    html = replace_all(std::move(html), "$$NOME_DELEGADO$$", document_html::escape_html(delegado.empty() ? "___________________" : delegado));
    html = replace_all(std::move(html), "$$PROCEDIMENTO$$", document_html::escape_html(procedimento));
    html = replace_all(std::move(html), "$$TIPO_PROCEDIMENTO$$", document_html::escape_html(tipo_procedimento));
    html = replace_all(std::move(html), "$$NUMERO_PROCEDIMENTO$$", document_html::escape_html(numero_procedimento));
    html = replace_all(std::move(html), "$$NOTA_WHISPER$$", footnote_span(note_whisper));
    html = replace_all(std::move(html), "$$NOTA_MODELO$$", footnote_span(note_model));
    return html;
}

std::string render_audio_entry_title_template(
    const std::string& tmpl,
    const std::string& numeral,
    const std::string& display_name,
    const std::string& description) {
    std::string description_parentheses;
    if (!description.empty()) description_parentheses = " (" + description + ")";
    std::string html = document_html::escape_html(tmpl);
    html = replace_all(std::move(html), "$$NUMERAL$$", document_html::escape_html(numeral));
    html = replace_all(std::move(html), "$$NOME_ARQUIVO$$", document_html::escape_html(display_name));
    html = replace_all(std::move(html), "$$DESCRICAO$$", document_html::escape_html(description));
    html = replace_all(std::move(html), "$$DESCRICAO_PARENTESES$$", document_html::escape_html(description_parentheses));
    return html;
}

void generate_audio_document(State& module) {
    const auto assets_dir = sto::files::executable_directory() / "assets" / "documents";
    document_html::DocumentAssets assets;
    std::string error;
    if (!document_html::load_default_assets(assets_dir, assets, error)) {
        module.set_note("Erro ao carregar assets: " + error, NoteKind::error);
        return;
    }

    // Load document template from DB (header, footer, logo)
    sto::database::DocumentsRepository doc_repo;
    sto::database::DocumentTemplate doc_tmpl;
    if (doc_repo.initialize(db_path(), error)) {
        doc_tmpl = doc_repo.load_template(error);
        if (!doc_tmpl.logo_path.empty()) {
            std::ifstream logo_f(doc_tmpl.logo_path, std::ios::binary);
            if (logo_f) {
                std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(logo_f)), {});
                if (!bytes.empty()) assets.logo_base64 = document_html::base64_encode(bytes);
            }
        }
    }

    // Build body
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    const std::chrono::year_month_day ymd{today};
    const std::string data_extenso = sto::text::format_data_extenso(
        static_cast<int>(static_cast<unsigned>(ymd.day())),
        static_cast<unsigned>(ymd.month()),
        static_cast<int>(ymd.year()));

    const std::string sigla = kProcedimentoSiglas[static_cast<std::size_t>(module.tipo_procedimento)];
    const std::string tipo  = kProcedimentoTypes[static_cast<std::size_t>(module.tipo_procedimento)];
    const std::string num   = module.numero_procedimento.data();
    const std::string proc  = tipo + " nº " + (num.empty() ? "—" : num);

    const std::string delegado = module.nome_delegado.data();

    const std::string audio_intro_template = doc_tmpl.audio_intro.empty()
        ? "Aos $$DATA_EXTENSO$$, por determinação do Excelentíssimo Senhor Delegado de Polícia Civil, "
          "$$NOME_DELEGADO$$, procedi à transcrição dos seguintes arquivos de áudio, transcritos "
          "NÃO NECESSARIAMENTE COM AS MESMAS PALAVRAS com o uso da ferramenta Whisper$$NOTA_WHISPER$$, "
          "modelo ggml-large-v3-turbo$$NOTA_MODELO$$, no âmbito $$PROCEDIMENTO$$:"
        : doc_tmpl.audio_intro;
    const std::string audio_entry_title_template = doc_tmpl.audio_entry_title.empty()
        ? "$$NUMERAL$$ – $$NOME_ARQUIVO$$$$DESCRICAO_PARENTESES$$:"
        : doc_tmpl.audio_entry_title;
    const std::string audio_closing_template = doc_tmpl.audio_closing.empty()
        ? "Nada mais havendo a constar, procedo ao encerramento das presentes transcrições de áudio, que vai devidamente assinado, na forma da Lei."
        : doc_tmpl.audio_closing;

    std::string body;
    body += "  <p class=\"justified\">";
    body += render_audio_intro_template(
        audio_intro_template, data_extenso, delegado, proc, tipo, num,
        doc_tmpl.audio_note_whisper, doc_tmpl.audio_note_model);
    body += "</p>\n\n";

    int idx = 0;
    for (const auto& s : module.sessions) {
        if (s->status != Status::completed || s->transcript.empty()) continue;
        ++idx;
        body += "  <div class=\"audio-entry\">\n";
        body += "    <div class=\"audio-title\">";
        body += render_audio_entry_title_template(
            audio_entry_title_template,
            roman_numeral(idx),
            s->display_name,
            s->description);
        body += "</div>\n";
        body += "    <div class=\"transcricao-box\">\n";
        body += "      <p class=\"justified\">";
        body += document_html::escape_html(s->transcript);
        body += "</p>\n    </div>\n  </div>\n\n";
    }

    if (idx == 0) {
        module.set_note("Nenhuma transcrição concluída para gerar o documento.", NoteKind::error);
        return;
    }

    // Load responsável info + save delegado name
    std::string nome_responsavel = module.nome_responsavel.data();
    std::string cargo_responsavel = module.cargo_responsavel.data();
    {
        std::string pref_err;
        auto prefs = doc_repo.load(pref_err);
        if (nome_responsavel.empty()) nome_responsavel = prefs.nome_responsavel;
        if (cargo_responsavel.empty()) cargo_responsavel = prefs.cargo_responsavel;
        // Persist delegado name if filled
        if ((!delegado.empty() && prefs.nome_delegado != delegado)
            || (!nome_responsavel.empty() && prefs.nome_responsavel != nome_responsavel)
            || (!cargo_responsavel.empty() && prefs.cargo_responsavel != cargo_responsavel)) {
            prefs.nome_delegado = delegado;
            prefs.nome_responsavel = nome_responsavel;
            prefs.cargo_responsavel = cargo_responsavel;
            doc_repo.save(prefs, pref_err);
        }
    }

    const std::size_t closing_start = body.size();
    body += "  <p class=\"closing\">Nada mais havendo a constar, procedo ao encerramento "
            "das presentes transcrições de áudio, que vai devidamente assinado, na forma da Lei.</p>\n\n";
    body.resize(closing_start);
    body += "  <p class=\"closing\">";
    body += document_html::escape_html(audio_closing_template);
    body += "</p>\n\n";
    if (!nome_responsavel.empty() || !cargo_responsavel.empty()) {
        body += "  <div class=\"signature\">\n    <div class=\"name\">";
        body += document_html::escape_html(nome_responsavel);
        body += "</div>\n    <div>";
        body += document_html::escape_html(cargo_responsavel);
        body += "</div>\n  </div>\n\n";
    }


    // Header HTML from template
    std::string header_html;
    if (!doc_tmpl.header_text.empty()) {
        std::istringstream hss(doc_tmpl.header_text);
        std::string line;
        while (std::getline(hss, line)) {
            if (!line.empty()) header_html += "<span class=\"line\">" + document_html::escape_html(line) + "</span>\n";
        }
    }

    // Build full document
    const std::string browser_title = "Auto de Transcrição de Áudios"
        + (sigla.empty() ? "" : " " + sigla)
        + (num.empty() ? "" : " " + num);

    document_html::PagedDocument doc;
    doc.browser_title  = browser_title;
    doc.document_title = doc_tmpl.audio_title.empty() ? "AUTO DE TRANSCRIÇÃO" : doc_tmpl.audio_title;
    doc.header_html    = header_html;
    doc.footer_html    = doc_tmpl.footer_text; // raw HTML
    doc.extra_style    = audio_css();
    doc.body_html      = body;
    const std::string html = document_html::build_paged_html(doc, assets);

    // Write and open
    const auto tmp_dir = document_html::temporary_directory();
    std::error_code tmp_ec;
    std::filesystem::create_directories(tmp_dir, tmp_ec);

    std::wstring filename = L"Auto de Transcricao de Audios";
    if (!sigla.empty()) filename += L" " + std::wstring(sigla.begin(), sigla.end());
    if (!num.empty())   filename += L" " + sto::files::sanitize_stem(std::wstring(num.begin(), num.end()));
    filename += L".html";
    const auto html_path = tmp_dir / filename;

    if (!document_html::write_html_file(html_path, html, error)) {
        module.set_note("Erro ao escrever arquivo: " + error, NoteKind::error);
        return;
    }
    if (!document_html::open_in_default_browser(html_path, error)) {
        module.set_note("Erro ao abrir navegador: " + error, NoteKind::error);
        return;
    }
    module.set_note("Documento aberto no navegador. Use Ctrl+P para salvar como PDF.");
}

// ---- UI ----

const char* status_icon(Status s) {
    switch (s) {
    case Status::completed: return kIconCheck;
    case Status::running:   return kIconGear;
    case Status::queued:    return kIconClock;
    case Status::failed:    return "\xef\x81\xaa"; // exclamation
    case Status::paused:    return "\xef\x81\x8c"; // pause
    default:                return "\xef\x80\x98"; // circle
    }
}

ImVec4 status_color(Status s) {
    switch (s) {
    case Status::completed: return kGreen;
    case Status::running:   return kBlue;
    case Status::queued:    return kYellow;
    case Status::failed:    return kError;
    case Status::paused:    return kMuted;
    default:                return kSubtle;
    }
}

// Calcula texto do tempo restante — idêntico ao TranscriptionModule
float visual_progress_audio(const Session& session) {
    const float reported = session.whisper_progress.load();
    if (!session.transcribing || reported >= 1.0F) return session.progress.load();

    const long long started = session.transcription_started_ms.load();
    if (started == 0) return reported;
    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const double elapsed = static_cast<double>(now_ms - started) / 1000.0;
    if (elapsed <= 0.0) return reported;

    const double projected_total = session.projected_total_seconds.load();
    if (projected_total <= 0.0) return reported;
    const float projected = static_cast<float>(elapsed / projected_total);
    return std::clamp(std::max(projected, reported), reported, 0.99F);
}

double remaining_seconds_audio(const Session& session) {
    if (!session.transcribing) return session.estimated_seconds.load();
    const long long started = session.transcription_started_ms.load();
    if (started == 0) return session.estimated_seconds.load();
    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const double elapsed = static_cast<double>(now_ms - started) / 1000.0;
    return std::max(0.0, session.projected_total_seconds.load() - elapsed);
}

std::string remaining_text_audio(const Session& session) {
    const double seconds = remaining_seconds_audio(session);
    if (session.transcribing && seconds <= 0.0 && session.whisper_progress.load() < 0.01F) return "calculando...";
    if (session.transcribing && seconds < 1.0 && session.progress.load() < 1.0F) return "quase pronto";
    return format_duration(seconds);
}

void render_station(State& module, Session& session) {
    const float avail_w   = ImGui::GetContentRegionAvail().x;
    const bool  is_running = session.status == Status::running;
    const bool  is_queued  = session.status == Status::queued;
    const ImGuiStyle& style = ImGui::GetStyle();

    // Nome + descrição
    ImGui::TextColored(kMuted, "Nome do arquivo");
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::InputText("##dn", &session.display_name))
        module.save_session(module.selected);
    ImGui::TextColored(kMuted, "Descrição");
    ImGui::SameLine(); ImGui::TextColored(kSubtle, "(opcional)");
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::InputTextWithHint("##desc", "Ex: Mensagem de voz de João, gravação da reunião...",
            &session.description))
        module.save_session(module.selected);
    ImGui::Spacing();
    ImGui::TextColored(kSilver, "TEXTO DA TRANSCRIÇÃO");
    ImGui::Spacing();

    // Espaço para botões + barra de progresso + separador
    constexpr float btn_h       = 42.0F;
    constexpr float progress_h  = 22.0F + 8.0F; // ProgressBar + Spacing
    const float     action_area = btn_h + style.ItemSpacing.y * 3.0F + 1.0F + 18.0F;
    const float     prog_area   = is_running ? (progress_h + style.ItemSpacing.y) : 0.0F;
    const float     transcript_h = std::max(42.0F, ImGui::GetContentRegionAvail().y - action_area - prog_area);

    // Caixa de transcrição — igual TranscriptionModule: mostra info de progresso DENTRO
    ImGui::BeginChild("audio-transcript", {0.0F, transcript_h}, ImGuiChildFlags_Borders);
    if (is_running || is_queued) {
        const int dots = static_cast<int>(ImGui::GetTime() * 2.0) % 5 + 1;
        ImGui::SetCursorPosY(std::max(12.0F, transcript_h * 0.17F));
        ImGui::SetWindowFontScale(1.18F);
        ImGui::TextColored(is_queued ? kYellow : kBlue, "%s", is_queued ? kIconClock : kIconGear);
        ImGui::SameLine();
        if (sto::ui::section_font()) ImGui::PushFont(sto::ui::section_font());
        if (is_queued) {
            ImGui::TextColored(kYellow, "Na fila aguardando transcrição");
        } else if (session.transcription_started_ms.load() > 0) {
            ImGui::TextColored(kBlue, "Transcrevendo");
            ImGui::TextColored(kSilver, "Tempo restante estimado: %s", remaining_text_audio(session).c_str());
        } else {
            ImGui::TextColored(kBlue, "Preparando arquivo de áudio");
        }
        ImGui::TextColored(kMuted, "Aguarde%s", std::string(static_cast<std::size_t>(dots), '.').c_str());
        if (sto::ui::section_font()) ImGui::PopFont();
        ImGui::SetWindowFontScale(1.00F);
    } else {
        ImGui::InputTextMultiline("##transcript-inner",
            &session.transcript,
            {-1.0F, -1.0F}, ImGuiInputTextFlags_WordWrap);
    }
    ImGui::EndChild();

    // Barra de progresso ABAIXO da caixa, ANTES dos botões — idêntico ao TranscriptionModule
    if (is_running) {
        ImGui::Spacing();
        ImGui::ProgressBar(visual_progress_audio(session), {-1.0F, progress_h - 8.0F});
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Botões — Transcrever | Gerar Auto | Limpar
    const float sp    = style.ItemSpacing.x;
    const float btn_w = (avail_w - sp * 2.0F) / 3.0F;
    const char* tr_text = is_running ? "Cancelar transcrição" : is_queued ? "Pausar na fila" : "Transcrever";

    ImGui::BeginDisabled(session.input.empty() && !is_running && !is_queued);
    if (colored_button(btn(is_running ? kIconCancel : is_queued ? kIconPause : kIconPlay, tr_text),
            {btn_w, btn_h}, is_running ? kYellow : kBlue, is_running ? kDark : kWhite)) {
        if (is_running) module.cancel_requested = true;
        else if (is_queued) {
            session.status = Status::paused;
            session.set_message("Transcrição pausada. Clique em Transcrever para reativar.");
            module.save_session(module.selected);
        }
        else if (!session.transcript.empty()) {
            module.retranscribe_confirmation = true;
        }
        else {
            session.status = Status::queued;
            module.save_session(module.selected);
            start_queue_worker(module);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(session.transcript.empty());
    if (colored_button(btn(kIconPdf, "Gerar Auto"), {btn_w, btn_h}, kGreen))
        module.show_gen_confirm = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(is_running || is_queued);
    if (colored_button(btn(kIconTrash, "Limpar conteúdo"), {btn_w, btn_h}, kRed)) {
        module.clear_confirmation = true;
    }
    ImGui::EndDisabled();

    if (module.retranscribe_confirmation) ImGui::OpenPopup("Confirmar nova transcrição");
    if (ImGui::BeginPopupModal("Confirmar nova transcrição", &module.retranscribe_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Já existe um texto transcrito. Deseja iniciar uma nova transcrição?");
        ImGui::TextColored(kSilver, "O conteúdo atual será substituído.");
        ImGui::Spacing();
        const float popup_spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttons_width = 150.0F + popup_spacing + 180.0F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (ImGui::GetContentRegionAvail().x - buttons_width) * 0.5F));
        if (ImGui::Button("Manter conteúdo", {150.0F, 36.0F})) module.retranscribe_confirmation = false;
        ImGui::SameLine();
        if (colored_button("Transcrever novamente", {180.0F, 36.0F}, kBlue)) {
            session.status = Status::queued;
            module.save_session(module.selected);
            module.retranscribe_confirmation = false;
            start_queue_worker(module);
        }
        ImGui::EndPopup();
    }
    if (module.clear_confirmation) ImGui::OpenPopup("Confirmar limpeza");
    if (ImGui::BeginPopupModal("Confirmar limpeza", &module.clear_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Deseja realmente limpar todo o conteúdo da transcrição?");
        ImGui::Spacing();
        const float popup_spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttons_width = 150.0F + popup_spacing + 150.0F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (ImGui::GetContentRegionAvail().x - buttons_width) * 0.5F));
        if (ImGui::Button("Manter conteúdo", {150.0F, 36.0F})) module.clear_confirmation = false;
        ImGui::SameLine();
        if (colored_button("Limpar conteúdo", {150.0F, 36.0F}, kRed)) {
            session.transcript.clear();
            session.status = Status::idle;
            module.save_session(module.selected);
            module.clear_confirmation = false;
        }
        ImGui::EndPopup();
    }
}

void render_queue_sidebar(State& module) {
    const float sidebar_w = module.sidebar_collapsed ? 52.0F : 270.0F;
    const float avail_h   = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("audio-sidebar", {sidebar_w, avail_h}, ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar);

    // Collapse toggle
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1,1,1,0.07F});
        ImGui::PushStyleColor(ImGuiCol_Text,          kMuted);
        if (ImGui::Button(module.sidebar_collapsed ? kIconChevronL : kIconChevronR, {28.0F, 28.0F}))
            module.sidebar_collapsed = !module.sidebar_collapsed;
        ImGui::PopStyleColor(3);
    }

    if (!module.sidebar_collapsed) {
        // Add files button
        if (colored_button(btn(kIconFolder, "Adicionar áudios"), {-1.0F, 34.0F}, kBlue)) {
            const auto files = sto::platform::select_files(L"Selecione arquivos de áudio", {
                {L"Arquivos de áudio", L"*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.opus;*.wma"},
                {L"Todos os arquivos", L"*.*"}
            });
            for (const auto& f : files) {
                sto::database::AudioSession as;
                as.display_name = narrow(f.filename().wstring());
                as.input_path   = narrow(f.wstring());
                std::string err;
                if (module.repository.create_session(as, err)) {
                    auto s = std::make_shared<Session>();
                    s->id = as.id;
                    s->input = f;
                    s->queue_order = as.queue_order;
                    s->display_name = as.display_name;
                    module.sessions.push_back(s);
                    if (!module.selected) module.selected = s;
                }
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // Session list — com multi-seleção (Ctrl/Shift/drag rubber-band)
    ImGui::BeginChild("audio-session-list", {0.0F, 0.0F}, false, ImGuiWindowFlags_None);

    const bool io_shift = ImGui::GetIO().KeyShift;
    const bool io_ctrl  = ImGui::GetIO().KeyCtrl;
    bool any_item_clicked = false;

    float item_y_offset = 0.0F;

    for (std::size_t i = 0; i < module.sessions.size(); ++i) {
        auto& s = module.sessions[i];
        ImGui::PushID(static_cast<int>(s->id));
        const bool sel = module.is_selected(s->id);

        if (module.sidebar_collapsed) {
            ImGui::PushStyleColor(ImGuiCol_Text, status_color(s->status));
            ImGui::Button(status_icon(s->status), {32.0F, 32.0F});
            if (ImGui::IsItemClicked()) module.set_single_selection(s);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s->display_name.c_str());
            ImGui::PopStyleColor();
        } else {
            // Row background
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            constexpr float row_h = 44.0F;
            const float row_w = ImGui::GetContentRegionAvail().x;

            item_y_offset += row_h + ImGui::GetStyle().ItemSpacing.y;
            ImGui::InvisibleButton("##row", {row_w - 36.0F, row_h});
            if (ImGui::IsItemClicked()) {
                any_item_clicked = true;
                module.handle_click(s->id, io_ctrl, io_shift);
            }
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(row_min, {row_min.x + row_w, row_min.y + row_h},
                ImGui::GetColorU32(sel     ? ImVec4{0.20F,0.21F,0.22F,1.F}
                                 : hovered ? ImVec4{0.13F,0.14F,0.15F,1.F}
                                           : ImVec4{0.07F,0.08F,0.09F,1.F}), 4.0F);
            draw->AddText({row_min.x + 8.0F, row_min.y + 6.0F},
                ImGui::GetColorU32(status_color(s->status)), status_icon(s->status));
            {
                std::string dname = s->display_name;
                if (dname.size() > 28) dname = dname.substr(0, 25) + "...";
                draw->AddText({row_min.x + 26.0F, row_min.y + 6.0F},
                    ImGui::GetColorU32(kWhite), dname.c_str());
            }
            // Barra de progresso fina na base do item quando rodando
            if (s->status == Status::running) {
                const float pf = visual_progress_audio(*s);
                draw->AddRectFilled(
                    {row_min.x, row_min.y + row_h - 3.0F},
                    {row_min.x + row_w * pf, row_min.y + row_h},
                    ImGui::GetColorU32(kBlue));
            }
            // Botão de deletar
            ImGui::SetCursorScreenPos({row_min.x + row_w - 32.0F, row_min.y + (row_h - 26.0F) * 0.5F});
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0,0,0,0});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1,1,1,0.08F});
            ImGui::PushStyleColor(ImGuiCol_Text,          kSubtle);
            if (ImGui::Button(kIconTrash, {26.0F, 26.0F}) && s->status != Status::running) {
                std::string err;
                module.repository.delete_session(s->id, err);
                if (module.selected == s) module.selected = nullptr;
                module.sessions.erase(module.sessions.begin() + static_cast<std::ptrdiff_t>(i));
                ImGui::PopStyleColor(3);
                ImGui::PopID();
                break;
            }
            ImGui::PopStyleColor(3);
            // Menu de contexto (clique direito)
            ImGui::SetCursorScreenPos(row_min);
            ImGui::InvisibleButton("##ctx-target", {row_w, row_h});
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                if (!module.is_selected(s->id)) module.set_single_selection(s);
                ImGui::OpenPopup("##ctx-menu");
            }
            if (ImGui::BeginPopup("##ctx-menu")) {
                const std::string suffix = module.queue_selection.size() > 1
                    ? std::format(" ({})", module.queue_selection.size())
                    : std::string{};
                if (ImGui::MenuItem((btn(kIconPlay, "Adicionar à fila") + suffix).c_str())) {
                    module.request_queue_selection();
                }
                if (ImGui::MenuItem((btn(kIconPause, "Pausar") + suffix).c_str())) {
                    module.request_pause_selection();
                }
                if (ImGui::MenuItem((btn(kIconTrash, "Remover") + suffix).c_str())) {
                    module.bulk_delete_confirmation = true;
                }
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos({row_min.x, row_min.y + row_h});
            ImGui::Spacing();
        }
        ImGui::PopID();
    }
    // Clicar em área vazia (sem Ctrl/Shift) deseleciona
    if (!any_item_clicked &&
        ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !io_ctrl && !io_shift) {
        module.queue_selection.clear();
        module.selected = nullptr;
    }
    ImGui::EndChild();
    if (module.bulk_delete_confirmation) ImGui::OpenPopup("Excluir áudios");
    if (ImGui::BeginPopupModal("Excluir áudios", &module.bulk_delete_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kWhite, "Remover %d item(ns) selecionado(s)?", static_cast<int>(module.queue_selection.size()));
        ImGui::Spacing();
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
        if (colored_button(btn(kIconTrash, "Remover"), {half, 36.0F}, kRed)) {
            module.remove_queue_selection();
            module.bulk_delete_confirmation = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (colored_button(btn(kIconCancel, "Cancelar"), {half, 36.0F}, kMuted)) {
            module.bulk_delete_confirmation = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (module.pause_running_confirmation) ImGui::OpenPopup("Confirmar pausa");
    if (ImGui::BeginPopupModal("Confirmar pausa", &module.pause_running_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kWhite, "A seleção inclui uma transcrição em andamento.");
        ImGui::TextColored(kMuted, "Pausar agora cancelará o processo atual.");
        ImGui::Spacing();
        if (colored_button(btn(kIconPause, "Pausar seleção"), {170.0F, 36.0F}, kYellow, kDark)) {
            module.confirm_pause_running();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (colored_button(btn(kIconCancel, "Manter"), {120.0F, 36.0F}, kMuted)) {
            module.pause_running_confirmation = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (module.requeue_overwrite_confirmation) ImGui::OpenPopup("Confirmar nova transcrição");
    if (ImGui::BeginPopupModal("Confirmar nova transcrição", &module.requeue_overwrite_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kWhite, "Algum item selecionado já possui transcrição.");
        ImGui::TextColored(kMuted, "Ao transcrever novamente, o texto atual será substituído.");
        ImGui::Spacing();
        if (colored_button(btn(kIconPlay, "Transcrever"), {150.0F, 36.0F}, kBlue)) {
            module.set_selection_status(Status::queued);
            module.requeue_overwrite_confirmation = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (colored_button(btn(kIconCancel, "Cancelar"), {120.0F, 36.0F}, kMuted)) {
            module.requeue_overwrite_confirmation = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

void save_document_preferences(State& module) {
    sto::database::DocumentsRepository repo;
    std::string err;
    if (!repo.initialize(db_path(), err)) return;
    auto prefs = repo.load(err);
    prefs.nome_delegado = module.nome_delegado.data();
    prefs.nome_responsavel = module.nome_responsavel.data();
    prefs.cargo_responsavel = module.cargo_responsavel.data();
    repo.save(prefs, err);
}

void render_procedure_bar(State& module) {
    constexpr float combo_w  = 240.0F;
    constexpr float num_w    = 160.0F;

    // Row 1: Delegado
    ImGui::TextColored(kMuted, "Delegado de Polícia Civil:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::InputTextWithHint("##delegado", "Nome completo do Delegado",
            module.nome_delegado.data(), module.nome_delegado.size())) {
        save_document_preferences(module);
    }
    ImGui::Spacing();

    // Row 2: Assinatura
    ImGui::TextColored(kMuted, "Assinatura:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.62F);
    if (ImGui::InputTextWithHint("##nome-responsavel", "Nome de quem assina",
            module.nome_responsavel.data(), module.nome_responsavel.size())) {
        save_document_preferences(module);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::InputTextWithHint("##cargo-responsavel", "Cargo",
            module.cargo_responsavel.data(), module.cargo_responsavel.size())) {
        save_document_preferences(module);
    }
    ImGui::Spacing();

    // Row 3: Procedimento
    ImGui::TextColored(kMuted, "Procedimento:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo_w);
    ImGui::Combo("##tipo-proc", &module.tipo_procedimento, kProcedimentoTypes.data(),
        static_cast<int>(kProcedimentoTypes.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(num_w);
    ImGui::InputTextWithHint("##num-proc", "Número", module.numero_procedimento.data(),
        module.numero_procedimento.size());
}

void render_gen_confirm(State& module) {
    if (module.show_gen_confirm) {
        ImGui::OpenPopup("##audio-gen-confirm");
        module.show_gen_confirm = false;
    }
    ImGui::SetNextWindowSize({400.0F, 0.0F}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##audio-gen-confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kWhite, "Gerar Auto de Transcrição de Áudios?");
        ImGui::Spacing();
        const int completed = static_cast<int>(std::count_if(module.sessions.begin(), module.sessions.end(),
            [](const auto& s){ return s->status == Status::completed && !s->transcript.empty(); }));
        ImGui::TextColored(kMuted, "O documento incluirá %d arquivo(s) com transcrição concluída.", completed);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
        if (colored_button(btn(kIconPdf, "Gerar"), {half, 38.0F}, kGreen)) {
            generate_audio_document(module);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (colored_button(btn(kIconCancel, "Cancelar"), {half, 38.0F}, kRed))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void render_note(State& module) {
    if (module.note_text.empty()) return;
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - module.note_time).count();
    if (elapsed > 5.0) { module.note_text.clear(); return; }
    const float alpha = static_cast<float>(std::clamp(1.0 - std::max(0.0, elapsed - 4.0), 0.0, 1.0));
    const ImVec4 accent = module.note_kind == NoteKind::error ? kError : kSuccess;
    const ImVec4 faded  = {accent.x, accent.y, accent.z, accent.w * alpha};
    const ImVec2 text_size = ImGui::CalcTextSize(module.note_text.c_str());
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width  = std::min(vp->WorkSize.x - 40.0F, text_size.x + 42.0F);
    const ImVec2 pos = {
        vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5F,
        vp->WorkPos.y + (vp->WorkSize.y - 46.0F) * 0.5F
    };
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(pos, {pos.x + width, pos.y + 46.0F},
        ImGui::GetColorU32(ImVec4{0.04F, 0.05F, 0.06F, 0.88F * alpha}), 8.0F);
    draw->AddRect(pos, {pos.x + width, pos.y + 46.0F},
        ImGui::GetColorU32(faded), 8.0F, 0, 1.2F);
    draw->AddText({pos.x + (width - text_size.x) * 0.5F, pos.y + (46.0F - text_size.y) * 0.5F},
        ImGui::GetColorU32(faded), module.note_text.c_str());
}

} // anonymous namespace

void render() {
    State& module = state();
    constexpr float margin = 10.0F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0F, 8.0F});
    ImGui::BeginChild("audio-module-frame",
        {ImGui::GetContentRegionAvail().x - margin, 0.0F}, false, ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(kSilver, "MÓDULOS  /  TRANSCRIÇÃO DE ÁUDIOS");
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    const char* title = "Transcrição de Áudios";
    const float tw = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - tw) * 0.5F));
    ImGui::TextColored(kWhite, "%s", title);
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();

    // Procedure bar (always visible at top)
    render_procedure_bar(module);
    ImGui::Separator();
    ImGui::Spacing();

    // Station + sidebar
    const float sidebar_w = module.sidebar_collapsed ? 52.0F : 270.0F;
    const float station_w = ImGui::GetContentRegionAvail().x - sidebar_w - ImGui::GetStyle().ItemSpacing.x;
    ImGui::BeginChild("audio-station-area", {station_w, 0.0F}, false, ImGuiWindowFlags_NoScrollbar);
    if (module.selected) {
        render_station(module, *module.selected);
    } else {
        ImGui::TextColored(kMuted, "Adicione arquivos de áudio na fila para começar.");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    render_queue_sidebar(module);

    // Auto-kick queue worker
    start_queue_worker(module);

    render_gen_confirm(module);
    render_note(module);

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace sto::modules::audio
