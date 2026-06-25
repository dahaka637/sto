#include "modules/utilities/UtilitiesModule.hpp"

#include "core/files/FileUtils.hpp"
#include "core/platform/FileDialogs.hpp"
#include "core/platform/ProcessRunner.hpp"
#include "imgui.h"
#include "ui/Theme.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <shellapi.h>

namespace sto::modules::utilities {
namespace {
constexpr const char* kIconBack = "\xef\x81\xa0";
constexpr const char* kIconVideo = "\xef\x87\x88";
constexpr const char* kIconAudio = "\xef\x87\x87";
constexpr const char* kIconPdf = "\xef\x87\x81";
constexpr const char* kIconWord = "\xef\x87\x82";
constexpr const char* kIconScissors = "\xef\x83\x84";
constexpr const char* kIconSignature = "\xef\x95\xb3";
constexpr const char* kIconFolder = "\xef\x81\xbb";
constexpr const char* kIconPlay = "\xef\x81\x8b";
constexpr const char* kIconCancel = "\xef\x81\x8d";
constexpr const char* kIconOpen = "\xef\x81\xbc";
constexpr const char* kIconTrash = "\xef\x87\xb8";
constexpr ImVec4 kWhite = {0.94F, 0.95F, 0.96F, 1.00F};
constexpr ImVec4 kSilver = {0.70F, 0.73F, 0.76F, 1.00F};
constexpr ImVec4 kMuted = {0.52F, 0.55F, 0.58F, 1.00F};
constexpr ImVec4 kSuccess = {0.20F, 1.00F, 0.36F, 1.00F};
constexpr ImVec4 kError = {1.00F, 0.22F, 0.22F, 1.00F};
constexpr ImVec4 kBlue = {0.00F, 0.48F, 1.00F, 1.00F};
constexpr ImVec4 kGreen = {0.16F, 0.65F, 0.27F, 1.00F};
constexpr ImVec4 kRed = {0.86F, 0.21F, 0.27F, 1.00F};
constexpr ImVec4 kYellow = {1.00F, 0.76F, 0.03F, 1.00F};
constexpr ImVec4 kPurple = {0.44F, 0.26F, 0.76F, 1.00F};
constexpr const char* kMergePdfPayload = "STO_MERGE_PDF";

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

void accent_line(float width, const ImVec4& color = kSilver) {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(position, {position.x + width, position.y + 3.0F}, ImGui::GetColorU32(color), 2.0F);
    ImGui::Dummy({width, 3.0F});
}

enum class Tool { dashboard, codec, audio, compress_pdf, split_pdf, range_split_pdf, merge_pdf, pdf_to_word, filenames };
enum class Status { idle, running, completed, failed, cancelled };

struct RenamePreview {
    std::filesystem::path source;
    std::filesystem::path destination;
};

struct PdfRangePart {
    int start = 1;
    int end = 1;
};

struct State {
    Tool tool = Tool::dashboard;
    std::atomic<Status> status = Status::idle;
    std::vector<std::filesystem::path> inputs;
    std::vector<RenamePreview> rename_preview;
    std::vector<int> split_page_counts;
    std::vector<PdfRangePart> pdf_range_parts;
    std::vector<int> merge_page_counts;
    std::jthread worker;
    std::atomic_bool cancel_requested = false;
    std::atomic<float> progress = 0.0F;
    std::mutex mutex;
    std::string message = "Selecione uma ferramenta para começar.";
    std::string current_item;
    std::filesystem::path output_directory;
    std::filesystem::path pdf_to_word_output;
    std::filesystem::path pdf_range_output_directory;
    std::filesystem::path merge_pdf_output;
    int pdf_level = 5;
    int split_parts = 3;
    int pdf_range_start = 1;
    int pdf_range_end = 1;
    int pdf_range_page_count = 0;
    int audio_format = 0;

    void set_message(std::string value, std::string item = {}) {
        std::scoped_lock lock(mutex);
        message = std::move(value);
        current_item = std::move(item);
    }

    std::pair<std::string, std::string> messages() {
        std::scoped_lock lock(mutex);
        return {message, current_item};
    }

    void set_output_directory(std::filesystem::path value) {
        std::scoped_lock lock(mutex);
        output_directory = std::move(value);
    }

    std::filesystem::path get_output_directory() {
        std::scoped_lock lock(mutex);
        return output_directory;
    }

    bool busy() const { return status == Status::running; }

    void begin(std::function<void()> operation) {
        if (busy()) return;
        if (worker.joinable()) worker.join();
        cancel_requested = false;
        progress = 0.0F;
        set_output_directory({});
        status = Status::running;
        worker = std::jthread([this, operation = std::move(operation)] {
            operation();
            if (status == Status::running) status = Status::completed;
            if (status == Status::completed) progress = 1.0F;
        });
    }
};

State& state() {
    static State instance;
    return instance;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, output.data(), size, nullptr, nullptr);
    output.resize(static_cast<std::size_t>(size - 1));
    return output;
}

std::filesystem::path runtime(const wchar_t* relative) {
    return sto::files::executable_directory() / relative;
}

std::filesystem::path ensure_extension(std::filesystem::path path, const std::wstring& extension) {
    if (path.extension().empty()) path += extension;
    return path;
}

bool same_existing_path(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    return std::filesystem::exists(left, error) && std::filesystem::exists(right, error) && std::filesystem::equivalent(left, right, error);
}

PdfRangePart normalized_range(PdfRangePart range, int page_count) {
    if (page_count <= 0) return {1, 1};
    range.start = std::clamp(range.start, 1, page_count);
    range.end = std::clamp(range.end, 1, page_count);
    if (range.start > range.end) std::swap(range.start, range.end);
    return range;
}

std::wstring range_file_suffix(const PdfRangePart& range) {
    if (range.start == range.end) return L"_pagina_" + std::to_wstring(range.start);
    return L"_paginas_" + std::to_wstring(range.start) + L"_a_" + std::to_wstring(range.end);
}

void reset_pdf_workflow(State& module) {
    module.inputs.clear();
    module.split_page_counts.clear();
    module.pdf_range_parts.clear();
    module.merge_page_counts.clear();
    module.pdf_to_word_output.clear();
    module.pdf_range_output_directory.clear();
    module.merge_pdf_output.clear();
    module.pdf_range_start = 1;
    module.pdf_range_end = 1;
    module.pdf_range_page_count = 0;
}

double probe_duration(const std::filesystem::path& input) {
    std::atomic_bool cancel = false;
    const auto result = sto::platform::run_process({
        runtime(L"runtime/ffmpeg/ffprobe.exe").wstring(), L"-v", L"error", L"-show_entries", L"format=duration",
        L"-of", L"default=nw=1:nk=1", input.wstring()
    }, cancel);
    try { return result.exit_code == 0 ? std::stod(result.output) : 0.0; }
    catch (...) { return 0.0; }
}

bool run_ffmpeg(State& module, const std::filesystem::path& input, const std::filesystem::path& output, const std::vector<std::wstring>& parameters, float base, float span) {
    const double duration = probe_duration(input);
    std::vector<std::wstring> command = {runtime(L"runtime/ffmpeg/ffmpeg.exe").wstring(), L"-hide_banner", L"-nostats", L"-y", L"-i", input.wstring()};
    command.insert(command.end(), parameters.begin(), parameters.end());
    command.insert(command.end(), {L"-progress", L"pipe:1", output.wstring()});
    const auto result = sto::platform::run_process(command, module.cancel_requested, [&](const std::string& line) {
        if (!line.starts_with("out_time_ms=") && !line.starts_with("out_time_us=")) return;
        try {
            const double current = std::stod(line.substr(line.find('=') + 1)) / 1'000'000.0;
            if (duration > 0.0) module.progress = base + span * static_cast<float>(std::clamp(current / duration, 0.0, 1.0));
        } catch (...) {}
    });
    if (result.cancelled) {
        module.status = Status::cancelled;
        module.set_message("Operação cancelada pelo usuário.");
        std::filesystem::remove(output);
        return false;
    }
    if (result.exit_code != 0) {
        module.status = Status::failed;
        module.set_message("FFmpeg não conseguiu processar um dos arquivos.", narrow(input.filename().wstring()));
        std::filesystem::remove(output);
        return false;
    }
    return true;
}

void run_codec_batch(State& module) {
    const auto count = static_cast<float>(module.inputs.size());
    for (std::size_t index = 0; index < module.inputs.size(); ++index) {
        const auto& input = module.inputs[index];
        module.set_message("Convertendo vídeo para padrão compatível...", narrow(input.filename().wstring()));
        const auto directory = input.parent_path() / L"codec_corrigido";
        std::filesystem::create_directories(directory);
        module.set_output_directory(directory);
        const auto output = sto::files::sanitized_output(input, directory, L".mp4");
        const float base = static_cast<float>(index) / count;
        if (!run_ffmpeg(module, input, output, {
            L"-map", L"0:v:0?", L"-map", L"0:a:0?", L"-sn", L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p",
            L"-preset", L"medium", L"-crf", L"21", L"-c:a", L"aac", L"-b:a", L"192k", L"-movflags", L"+faststart"
        }, base, 1.0F / count)) return;
    }
    module.set_message("Vídeos processados com sucesso.");
}

void run_audio_batch(State& module) {
    static const std::array extensions = {L".mp3", L".wav", L".flac", L".m4a"};
    const auto count = static_cast<float>(module.inputs.size());
    for (std::size_t index = 0; index < module.inputs.size(); ++index) {
        const auto& input = module.inputs[index];
        module.set_message("Convertendo arquivo de áudio...", narrow(input.filename().wstring()));
        const auto directory = input.parent_path() / L"audios_convertidos";
        std::filesystem::create_directories(directory);
        module.set_output_directory(directory);
        const auto output = sto::files::sanitized_output(input, directory, extensions[module.audio_format]);
        std::vector<std::wstring> arguments{L"-vn"};
        if (module.audio_format == 0) arguments.insert(arguments.end(), {L"-c:a", L"libmp3lame", L"-b:a", L"192k", L"-ar", L"44100", L"-ac", L"2"});
        else if (module.audio_format == 1) arguments.insert(arguments.end(), {L"-c:a", L"pcm_s16le"});
        else if (module.audio_format == 2) arguments.insert(arguments.end(), {L"-c:a", L"flac"});
        else arguments.insert(arguments.end(), {L"-c:a", L"aac", L"-b:a", L"192k"});
        if (!run_ffmpeg(module, input, output, arguments, static_cast<float>(index) / count, 1.0F / count)) return;
    }
    module.set_message("Áudios convertidos com sucesso.");
}

int pdf_page_count(const std::filesystem::path& input) {
    std::atomic_bool cancel = false;
    const auto result = sto::platform::run_process({
        runtime(L"runtime/ghostscript/gswin64c.exe").wstring(), L"-q", L"-dNODISPLAY", L"-dSAFER",
        L"--permit-file-read=" + input.wstring(), L"-sPDFname=" + input.wstring(),
        L"-c", L"PDFname (r) file runpdfbegin pdfpagecount = quit"
    }, cancel);
    try { return result.exit_code == 0 ? std::stoi(result.output) : 0; }
    catch (...) { return 0; }
}

std::vector<std::wstring> compression_arguments(const std::filesystem::path& input, const std::filesystem::path& output, int level) {
    const int dpi = static_cast<int>(300 - ((level - 1) * (250.0 / 9.0)));
    const wchar_t* setting = level >= 8 ? L"/screen" : level >= 5 ? L"/ebook" : level >= 2 ? L"/printer" : L"/prepress";
    return {
        runtime(L"runtime/ghostscript/gswin64c.exe").wstring(), L"-sDEVICE=pdfwrite", L"-dCompatibilityLevel=1.4",
        L"-dPDFSETTINGS=" + std::wstring(setting), L"-dNOPAUSE", L"-dQUIET", L"-dBATCH", L"-dSAFER",
        L"-dDetectDuplicateImages=true", L"-dColorImageResolution=" + std::to_wstring(dpi),
        L"-dGrayImageResolution=" + std::to_wstring(dpi), L"-dMonoImageResolution=" + std::to_wstring(dpi),
        L"-sOutputFile=" + output.wstring(), input.wstring()
    };
}

void run_pdf_compression(State& module) {
    const auto count = static_cast<float>(module.inputs.size());
    for (std::size_t index = 0; index < module.inputs.size(); ++index) {
        const auto& input = module.inputs[index];
        const auto directory = input.parent_path() / L"pdfs_comprimidos";
        std::filesystem::create_directories(directory);
        module.set_output_directory(directory);
        const auto output = sto::files::sanitized_output(input, directory, L".pdf");
        const auto temporary = output.parent_path() / (output.stem().wstring() + L".sto.tmp.pdf");
        const double original_size = sto::files::size_megabytes(input);
        bool success = false;
        for (int level = module.pdf_level; level <= 10; ++level) {
            module.set_message(std::format("Comprimindo PDF no nível {}...", level), narrow(input.filename().wstring()));
            module.progress = (static_cast<float>(index) + static_cast<float>(level - module.pdf_level) / static_cast<float>(11 - module.pdf_level)) / count;
            std::filesystem::remove(temporary);
            const auto result = sto::platform::run_process(compression_arguments(input, temporary, level), module.cancel_requested);
            if (result.cancelled) {
                module.status = Status::cancelled;
                module.set_message("Operação cancelada pelo usuário.");
                std::filesystem::remove(temporary);
                return;
            }
            if (result.exit_code != 0) {
                module.status = Status::failed;
                module.set_message("Ghostscript não conseguiu comprimir o PDF.", narrow(input.filename().wstring()));
                std::filesystem::remove(temporary);
                return;
            }
            const double compressed_size = sto::files::size_megabytes(temporary);
            if (compressed_size < original_size * 0.98 || level == 10) {
                std::filesystem::rename(temporary, output);
                success = true;
                break;
            }
        }
        if (!success) std::filesystem::remove(temporary);
    }
    module.set_message("PDFs comprimidos com sucesso.");
}

void run_pdf_split(State& module) {
    const auto count = static_cast<float>(module.inputs.size());
    for (std::size_t file_index = 0; file_index < module.inputs.size(); ++file_index) {
        const auto& input = module.inputs[file_index];
        const int pages = pdf_page_count(input);
        if (pages <= 0) {
            module.status = Status::failed;
            module.set_message("Não foi possível identificar o número de páginas.", narrow(input.filename().wstring()));
            return;
        }
        const int parts = std::min(pages, module.split_parts);
        const auto directory = input.parent_path() / (L"pdf_dividido_" + sto::files::sanitize_stem(input.stem().wstring()));
        std::filesystem::create_directories(directory);
        module.set_output_directory(directory);
        int start = 1;
        for (int part = 1; part <= parts; ++part) {
            const int length = pages / parts + (part <= pages % parts ? 1 : 0);
            const int end = start + length - 1;
            module.set_message(std::format("Gerando parte {} de {}...", part, parts), narrow(input.filename().wstring()));
            module.progress = (static_cast<float>(file_index) + static_cast<float>(part - 1) / static_cast<float>(parts)) / count;
            const auto output = sto::files::unique_path(directory / (sto::files::sanitize_stem(input.stem().wstring()) + L"_parte_" + std::to_wstring(part) + L".pdf"));
            const auto result = sto::platform::run_process({
                runtime(L"runtime/ghostscript/gswin64c.exe").wstring(), L"-sDEVICE=pdfwrite", L"-dNOPAUSE", L"-dBATCH", L"-dSAFER", L"-dQUIET",
                L"-dFirstPage=" + std::to_wstring(start), L"-dLastPage=" + std::to_wstring(end), L"-sOutputFile=" + output.wstring(), input.wstring()
            }, module.cancel_requested);
            if (result.cancelled) {
                module.status = Status::cancelled;
                module.set_message("Operação cancelada pelo usuário.");
                std::filesystem::remove(output);
                return;
            }
            if (result.exit_code != 0) {
                module.status = Status::failed;
                module.set_message("Ghostscript não conseguiu dividir o PDF.", narrow(input.filename().wstring()));
                std::filesystem::remove(output);
                return;
            }
            start = end + 1;
        }
    }
    module.set_message("PDFs divididos com sucesso.");
}

void run_pdf_range_split(State& module) {
    const auto ghostscript = runtime(L"runtime/ghostscript/gswin64c.exe");
    if (!std::filesystem::exists(ghostscript)) {
        module.status = Status::failed;
        module.set_message("Runtime Ghostscript não encontrado.", "Verifique runtime/ghostscript/gswin64c.exe");
        return;
    }
    if (module.inputs.empty() || module.pdf_range_parts.empty() || module.pdf_range_output_directory.empty()) {
        module.status = Status::failed;
        module.set_message("Selecione um PDF, adicione os intervalos e escolha a pasta de destino.");
        return;
    }

    const auto input = module.inputs.front();
    const int pages = module.pdf_range_page_count > 0 ? module.pdf_range_page_count : pdf_page_count(input);
    if (pages <= 0) {
        module.status = Status::failed;
        module.set_message("Não foi possível identificar o número de páginas.", narrow(input.filename().wstring()));
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(module.pdf_range_output_directory, error);
    if (error) {
        module.status = Status::failed;
        module.set_message("Não foi possível criar a pasta de destino.", error.message());
        return;
    }

    module.set_output_directory(module.pdf_range_output_directory);
    const auto count = static_cast<float>(module.pdf_range_parts.size());
    for (std::size_t index = 0; index < module.pdf_range_parts.size(); ++index) {
        const PdfRangePart range = normalized_range(module.pdf_range_parts[index], pages);
        const auto output = sto::files::unique_path(
            module.pdf_range_output_directory / (sto::files::sanitize_stem(input.stem().wstring()) + range_file_suffix(range) + L".pdf")
        );

        module.set_message(std::format("Gerando parte {} de {}...", index + 1, module.pdf_range_parts.size()),
            std::format("Páginas {} a {}", range.start, range.end));
        module.progress = static_cast<float>(index) / count;

        const auto result = sto::platform::run_process({
            ghostscript.wstring(), L"-sDEVICE=pdfwrite", L"-dNOPAUSE", L"-dBATCH", L"-dSAFER", L"-dQUIET",
            L"-dFirstPage=" + std::to_wstring(range.start), L"-dLastPage=" + std::to_wstring(range.end),
            L"-sOutputFile=" + output.wstring(), input.wstring()
        }, module.cancel_requested, {}, module.pdf_range_output_directory.wstring());

        if (result.cancelled) {
            module.status = Status::cancelled;
            module.set_message("Operação cancelada pelo usuário.");
            std::filesystem::remove(output);
            return;
        }
        if (!result.started || result.exit_code != 0 || !std::filesystem::exists(output)) {
            module.status = Status::failed;
            module.set_message("Ghostscript não conseguiu separar o PDF.", narrow(input.filename().wstring()));
            std::filesystem::remove(output);
            return;
        }
        module.progress = static_cast<float>(index + 1) / count;
    }

    module.set_message("PDF separado em intervalos com sucesso.");
}

void run_pdf_merge(State& module) {
    const auto ghostscript = runtime(L"runtime/ghostscript/gswin64c.exe");
    if (!std::filesystem::exists(ghostscript)) {
        module.status = Status::failed;
        module.set_message("Runtime Ghostscript não encontrado.", "Verifique runtime/ghostscript/gswin64c.exe");
        return;
    }
    if (module.inputs.size() < 2 || module.merge_pdf_output.empty()) {
        module.status = Status::failed;
        module.set_message("Selecione ao menos dois PDFs e o arquivo de destino.");
        return;
    }

    const auto output = ensure_extension(module.merge_pdf_output, L".pdf");
    for (const auto& input : module.inputs) {
        if (same_existing_path(input, output)) {
            module.status = Status::failed;
            module.set_message("O PDF de destino não pode substituir um dos arquivos de origem.", narrow(input.filename().wstring()));
            return;
        }
    }

    std::error_code error;
    std::filesystem::create_directories(output.parent_path(), error);
    if (error) {
        module.status = Status::failed;
        module.set_message("Não foi possível criar a pasta de destino.", error.message());
        return;
    }

    module.set_output_directory(output.parent_path());
    module.set_message("Juntando PDFs...", narrow(output.filename().wstring()));
    module.progress = 0.0F;

    std::vector<std::wstring> command = {
        ghostscript.wstring(), L"-sDEVICE=pdfwrite", L"-dCompatibilityLevel=1.4",
        L"-dNOPAUSE", L"-dBATCH", L"-dSAFER", L"-dQUIET",
        L"-sOutputFile=" + output.wstring()
    };
    for (const auto& input : module.inputs) command.push_back(input.wstring());

    const auto result = sto::platform::run_process(command, module.cancel_requested, {}, output.parent_path().wstring());
    if (result.cancelled) {
        module.status = Status::cancelled;
        module.set_message("Operação cancelada pelo usuário.");
        std::filesystem::remove(output);
        return;
    }
    if (!result.started || result.exit_code != 0 || !std::filesystem::exists(output)) {
        module.status = Status::failed;
        module.set_message("Ghostscript não conseguiu juntar os PDFs.", narrow(output.filename().wstring()));
        std::filesystem::remove(output);
        return;
    }

    module.progress = 1.0F;
    module.set_message("PDFs juntados com sucesso.", narrow(output.filename().wstring()));
}

void run_pdf_to_word(State& module) {
    const auto converter = runtime(L"runtime/pdf2docx/pdf2docx.exe");
    if (!std::filesystem::exists(converter)) {
        module.status = Status::failed;
        module.set_message("Runtime PDF para Word não encontrado.", "Verifique runtime/pdf2docx/pdf2docx.exe");
        return;
    }

    if (module.inputs.empty() || module.pdf_to_word_output.empty()) {
        module.status = Status::failed;
        module.set_message("Selecione o PDF e o local onde o Word será salvo.");
        return;
    }

    const auto input = module.inputs.front();
    const auto output = ensure_extension(module.pdf_to_word_output, L".docx");
    module.set_output_directory(output.parent_path());
    module.set_message("Convertendo PDF para Word...", narrow(input.filename().wstring()));
    module.progress = 0.0F;

    const auto result = sto::platform::run_process({
        converter.wstring(), L"convert", input.wstring(), output.wstring()
    }, module.cancel_requested, {}, output.parent_path().wstring());

    if (result.cancelled) {
        module.status = Status::cancelled;
        module.set_message("Operação cancelada pelo usuário.");
        std::filesystem::remove(output);
        return;
    }
    if (!result.started) {
        module.status = Status::failed;
        module.set_message("Não foi possível iniciar o conversor PDF para Word.", narrow(input.filename().wstring()));
        std::filesystem::remove(output);
        return;
    }
    if (result.exit_code != 0 || !std::filesystem::exists(output)) {
        module.status = Status::failed;
        module.set_message("Não foi possível converter o PDF. Se ele for digitalizado, aguarde a versão com OCR.", narrow(input.filename().wstring()));
        std::filesystem::remove(output);
        return;
    }

    module.progress = 1.0F;
    module.set_message("Documento Word gerado com sucesso.", narrow(output.filename().wstring()));
}

void build_rename_preview(State& module, const std::filesystem::path& directory) {
    module.rename_preview.clear();
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        const auto& source = entry.path();
        const auto destination = source.parent_path() / (sto::files::sanitize_stem(source.stem().wstring()) + source.extension().wstring());
        if (source.filename() != destination.filename()) module.rename_preview.push_back({source, sto::files::unique_path(destination)});
    }
}

void run_rename(State& module) {
    const float count = static_cast<float>(module.rename_preview.size());
    for (std::size_t index = 0; index < module.rename_preview.size(); ++index) {
        const auto& item = module.rename_preview[index];
        module.set_message("Renomeando arquivos...", narrow(item.source.filename().wstring()));
        std::error_code error;
        std::filesystem::rename(item.source, item.destination, error);
        if (error) {
            module.status = Status::failed;
            module.set_message("Não foi possível renomear um arquivo.", narrow(item.source.filename().wstring()));
            return;
        }
        module.progress = static_cast<float>(index + 1) / count;
    }
    if (!module.rename_preview.empty()) module.set_output_directory(module.rename_preview.front().source.parent_path());
    module.set_message("Nomes corrigidos com sucesso.");
}

void open_directory(const std::filesystem::path& directory) {
    if (!directory.empty() && std::filesystem::exists(directory)) {
        ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void render_progress(State& module) {
    const auto [message, item] = module.messages();
    const Status status = module.status.load();
    if (status == Status::idle) return;
    const ImVec4 status_color = status == Status::failed ? kError : status == Status::completed ? kSuccess : kSilver;
    const ImVec4 accent_color = status == Status::failed ? kRed : status == Status::completed ? kGreen : kBlue;
    const float available_width = ImGui::GetContentRegionAvail().x;
    const float content_width = std::min(available_width, 520.0F);
    const float centered_x = std::max(0.0F, (available_width - content_width) * 0.5F);
    const auto center_text = [&](const std::string& text, const ImVec4& color) {
        if (text.empty()) return;
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (available_width - size.x) * 0.5F));
        ImGui::TextColored(color, "%s", text.c_str());
    };

    ImGui::Dummy({0.0F, 12.0F});
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (available_width - 48.0F) * 0.5F));
    accent_line(48.0F, accent_color);
    ImGui::Spacing();
    center_text(message, status_color);
    center_text(item, kMuted);
    if (status == Status::running) {
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centered_x);
        ImGui::ProgressBar(module.progress.load(), {content_width, 20.0F});
        ImGui::Spacing();
        constexpr float button_width = 210.0F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (available_width - button_width) * 0.5F));
        if (colored_button(label(kIconCancel, "Cancelar operação"), {button_width, 36.0F}, kYellow, {0.05F, 0.06F, 0.07F, 1.0F})) module.cancel_requested = true;
    } else if (status == Status::completed) {
        const auto destination = module.get_output_directory();
        ImGui::Spacing();
        if (!destination.empty()) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (available_width - 240.0F) * 0.5F));
            if (colored_button(label(kIconOpen, "Abrir pasta de destino"), {240.0F, 36.0F}, kGreen)) {
                open_directory(destination);
            }
        }
    }
    ImGui::Dummy({0.0F, 6.0F});
}

void render_selected_files(const State& module) {
    ImGui::TextColored(kMuted, "Arquivos selecionados: %zu", module.inputs.size());
    ImGui::BeginChild("selected-files", {0.0F, 124.0F}, ImGuiChildFlags_Borders);
    if (module.inputs.empty()) {
        ImGui::TextColored(kMuted, "Nenhum arquivo selecionado.");
    } else {
        for (const auto& file : module.inputs) ImGui::TextColored(kSilver, "%s", narrow(file.filename().wstring()).c_str());
    }
    ImGui::EndChild();
}

void render_standard_tool(
    State& module,
    const char* title,
    const char* description,
    const std::vector<sto::platform::FileFilter>& filters,
    const std::function<void(State&)>& execute,
    const std::function<void()>& render_options = {}
) {
    if (!module.busy() && colored_button(label(kIconBack, "Voltar"), {112.0F, 34.0F}, ImVec4{0.14F, 0.15F, 0.16F, 1.0F})) module.tool = Tool::dashboard;
    ImGui::Spacing();
    ImGui::BeginChild("tool-header", {0.0F, 116.0F}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    accent_line(52.0F, kBlue);
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "%s", title);
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::TextColored(kMuted, "%s", description);
    ImGui::EndChild();
    ImGui::Spacing();
    if (render_options) {
        ImGui::BeginChild("tool-options", {0.0F, 70.0F}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        render_options();
        ImGui::EndChild();
        ImGui::Spacing();
    }
    if (!module.busy() && colored_button(label(kIconFolder, "Selecionar arquivos"), {220.0F, 38.0F}, kBlue)) {
        module.inputs = sto::platform::select_files(L"Selecione os arquivos", filters);
        module.status = Status::idle;
        module.progress = 0.0F;
        module.set_output_directory({});
    }
    ImGui::Spacing();
    render_selected_files(module);
    ImGui::Spacing();
    if (!module.busy() && !module.inputs.empty() && colored_button(label(kIconPlay, "Iniciar processamento"), {-1.0F, 42.0F}, kGreen)) module.begin([&module, execute] { execute(module); });
    render_progress(module);
}

void render_dashboard(State& module) {
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "Ferramentas Diversas");
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::Spacing();
    struct Card { Tool tool; const char* icon; const char* title; const char* description; ImVec4 color; };
    constexpr std::array cards = {
        Card{Tool::codec, kIconVideo, "Correção de Codec de Vídeos", "Compatibilidade de vídeos com sistemas processuais.", kBlue},
        Card{Tool::audio, kIconAudio, "Conversão de Áudios", "Conversão em lote com MP3 como formato padrão.", kGreen},
        Card{Tool::compress_pdf, kIconPdf, "Compressor Inteligente de PDF", "Compressão progressiva de documentos PDF.", kPurple},
        Card{Tool::split_pdf, kIconScissors, "Divisor Automático de PDF", "Divisão equilibrada por quantidade de páginas.", kYellow},
        Card{Tool::range_split_pdf, kIconScissors, "Separador de PDF", "Partes definidas por intervalos específicos de páginas.", kYellow},
        Card{Tool::merge_pdf, kIconPdf, "Juntar PDFs", "Organização e união de vários PDFs em um único arquivo.", kPurple},
        Card{Tool::pdf_to_word, kIconWord, "PDF para Word", "Conversão de PDFs digitais para arquivos DOCX editáveis.", kBlue},
        Card{Tool::filenames, kIconSignature, "Correção de Nomes", "Padronização segura de nomes em lote.", kSilver},
    };
    if (ImGui::BeginTable("utility-cards", 3, ImGuiTableFlags_SizingStretchSame)) {
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
            ImGui::TextColored(kMuted, "%s", card.description);
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 44.0F);
            if (colored_button("Abrir##" + std::string(card.title), {-1.0F, 32.0F}, card.color, card.color.x > 0.9F ? ImVec4{0.05F, 0.06F, 0.07F, 1.0F} : kWhite)) {
                module.tool = card.tool;
                module.rename_preview.clear();
                reset_pdf_workflow(module);
                module.status = Status::idle;
                module.progress = 0.0F;
                module.set_message("Selecione os arquivos para iniciar.");
            }
            ImGui::EndChild();
        }
        ImGui::EndTable();
    }
}

void render_audio(State& module) {
    render_standard_tool(module, "Conversão de Áudios", "Converta múltiplos formatos de áudio em lote. O padrão recomendado é MP3.", {
        {L"Arquivos de áudio", L"*.ogg;*.wav;*.m4a;*.flac;*.mp3;*.wma;*.aac;*.opus"},
        {L"Todos os arquivos", L"*.*"}
    }, run_audio_batch, [&module] {
        if (module.busy()) return;
        const char* formats[] = {"MP3 - recomendado", "WAV", "FLAC", "M4A / AAC"};
        ImGui::SetNextItemWidth(220.0F);
        ImGui::Combo("Formato de saída", &module.audio_format, formats, 4);
    });
}

void render_compress_pdf(State& module) {
    render_standard_tool(module, "Compressor Inteligente de PDF", "Comprima PDFs em lote com tentativas progressivas quando necessário.", {{L"Arquivos PDF", L"*.pdf"}}, run_pdf_compression, [&module] {
        if (module.busy()) return;
        ImGui::SetNextItemWidth(220.0F);
        ImGui::SliderInt("Nível inicial", &module.pdf_level, 1, 10);
    });
}

void render_split_pdf(State& module) {
    if (!module.busy() && colored_button(label(kIconBack, "Voltar"), {112.0F, 34.0F}, ImVec4{0.14F, 0.15F, 0.16F, 1.0F})) module.tool = Tool::dashboard;
    ImGui::Spacing();
    ImGui::BeginChild("split-header", {0.0F, 116.0F}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    accent_line(52.0F, kYellow);
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "Divisor Automático de PDF");
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::TextColored(kMuted, "Selecione os PDFs, confira as páginas detectadas e defina quantas partes equilibradas deseja gerar.");
    ImGui::EndChild();
    ImGui::Spacing();

    if (!module.busy() && colored_button(label(kIconFolder, "Selecionar arquivos"), {220.0F, 38.0F}, kBlue)) {
        module.inputs = sto::platform::select_files(L"Selecione os PDFs", {{L"Arquivos PDF", L"*.pdf"}});
        module.split_page_counts.clear();
        module.status = Status::idle;
        module.progress = 0.0F;
        module.set_output_directory({});

        bool valid = !module.inputs.empty();
        for (const auto& input : module.inputs) {
            const int pages = pdf_page_count(input);
            module.split_page_counts.push_back(pages);
            valid = valid && pages > 0;
        }

        if (!valid && !module.inputs.empty()) {
            module.status = Status::failed;
            module.set_message("Não foi possível identificar o número de páginas de um PDF.");
        } else if (!module.split_page_counts.empty()) {
            const int maximum = *std::ranges::min_element(module.split_page_counts);
            module.split_parts = maximum >= 2 ? std::clamp(module.split_parts, 2, maximum) : 2;
        }
    }

    if (!module.inputs.empty()) {
        ImGui::Spacing();
        render_selected_files(module);
        const bool pages_valid = !module.split_page_counts.empty()
            && std::ranges::all_of(module.split_page_counts, [](int pages) { return pages > 0; });
        if (pages_valid) {
            const int maximum = *std::ranges::min_element(module.split_page_counts);
            ImGui::Spacing();
            ImGui::BeginChild("detected-pages", {0.0F, 100.0F}, ImGuiChildFlags_Borders);
            ImGui::TextColored(kSilver, "PÁGINAS DETECTADAS");
            for (std::size_t index = 0; index < module.inputs.size(); ++index) {
                ImGui::TextColored(kMuted, "%s: %d página(s)", narrow(module.inputs[index].filename().wstring()).c_str(), module.split_page_counts[index]);
            }
            ImGui::EndChild();
            ImGui::Spacing();
            if (maximum >= 2) {
                ImGui::SetNextItemWidth(260.0F);
                ImGui::SliderInt("Quantidade de partes", &module.split_parts, 2, maximum);
                ImGui::Spacing();
                if (!module.busy() && colored_button(label(kIconPlay, "Iniciar divisão"), {-1.0F, 42.0F}, kGreen)) {
                    module.begin([&module] { run_pdf_split(module); });
                }
            } else {
                ImGui::TextColored(kError, "O PDF precisa ter ao menos duas páginas para ser dividido.");
            }
        }
    }
    render_progress(module);
}

void render_range_split_pdf(State& module) {
    if (!module.busy() && colored_button(label(kIconBack, "Voltar"), {112.0F, 34.0F}, ImVec4{0.14F, 0.15F, 0.16F, 1.0F})) module.tool = Tool::dashboard;
    ImGui::Spacing();
    ImGui::BeginChild("range-split-header", {0.0F, 96.0F}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    accent_line(52.0F, kYellow);
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "Separador de PDF");
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::TextColored(kMuted, "Escolha um PDF, adicione os intervalos desejados e gere as partes.");
    ImGui::EndChild();
    ImGui::Spacing();

    if (!module.busy() && colored_button(label(kIconFolder, module.inputs.empty() ? "Selecionar PDF" : "Trocar PDF"), {190.0F, 38.0F}, kBlue)) {
        module.inputs = sto::platform::select_files(L"Selecione o PDF", {{L"Arquivos PDF", L"*.pdf"}});
        if (module.inputs.size() > 1) module.inputs.resize(1);
        module.pdf_range_parts.clear();
        module.pdf_range_output_directory.clear();
        module.pdf_range_page_count = 0;
        module.pdf_range_start = 1;
        module.pdf_range_end = 1;
        module.status = Status::idle;
        module.progress = 0.0F;
        module.set_output_directory({});
        if (!module.inputs.empty()) {
            module.pdf_range_page_count = pdf_page_count(module.inputs.front());
            module.pdf_range_end = module.pdf_range_page_count > 0 ? module.pdf_range_page_count : 1;
            if (module.pdf_range_page_count <= 0) {
                module.status = Status::failed;
                module.set_message("Não foi possível identificar o número de páginas.", narrow(module.inputs.front().filename().wstring()));
            }
        }
    }

    ImGui::Spacing();
    if (module.inputs.empty()) {
        ImGui::TextColored(kMuted, "Nenhum PDF selecionado.");
    } else {
        ImGui::TextColored(kSilver, "%s", narrow(module.inputs.front().filename().wstring()).c_str());
    }

    if (!module.inputs.empty() && module.pdf_range_page_count > 0) {
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "Total: %d página(s)", module.pdf_range_page_count);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kSilver, "Intervalo:");
        ImGui::SameLine(0.0F, 10.0F);
        ImGui::SetNextItemWidth(96.0F);
        ImGui::InputInt("##range-start", &module.pdf_range_start, 0, 0);
        ImGui::SameLine(0.0F, 10.0F);
        ImGui::TextColored(kSilver, "-");
        ImGui::SameLine(0.0F, 10.0F);
        ImGui::SetNextItemWidth(96.0F);
        ImGui::InputInt("##range-end", &module.pdf_range_end, 0, 0);
        const PdfRangePart current = normalized_range({module.pdf_range_start, module.pdf_range_end}, module.pdf_range_page_count);
        module.pdf_range_start = current.start;
        module.pdf_range_end = current.end;
        ImGui::SameLine(0.0F, 14.0F);
        if (!module.busy() && colored_button("Adicionar", {116.0F, 32.0F}, kGreen)) {
            module.pdf_range_parts.push_back(current);
        }

        ImGui::Spacing();
        ImGui::TextColored(kSilver, "Partes a gerar");
        ImGui::BeginChild("configured-ranges", {0.0F, 184.0F}, ImGuiChildFlags_Borders);
        if (module.pdf_range_parts.empty()) {
            ImGui::TextColored(kMuted, "Adicione um ou mais intervalos.");
        } else if (ImGui::BeginTable("range-table", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36.0F);
            ImGui::TableSetupColumn("Intervalo");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 42.0F);
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < module.pdf_range_parts.size(); ++index) {
                PdfRangePart& part = module.pdf_range_parts[index];
                part = normalized_range(part, module.pdf_range_page_count);
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(kSilver, "%zu", index + 1);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(kMuted, "Página %d até Página %d (%d página(s))", part.start, part.end, part.end - part.start + 1);
                ImGui::TableSetColumnIndex(2);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1.0F, 1.0F, 1.0F, 0.06F});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{1.0F, 1.0F, 1.0F, 0.10F});
                ImGui::PushStyleColor(ImGuiCol_Text, kError);
                if (!module.busy() && ImGui::SmallButton(kIconTrash)) {
                    module.pdf_range_parts.erase(module.pdf_range_parts.begin() + static_cast<std::ptrdiff_t>(index));
                    ImGui::PopStyleColor(4);
                    ImGui::PopID();
                    break;
                }
                ImGui::PopStyleColor(4);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remover intervalo");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (!module.busy() && !module.pdf_range_parts.empty() && colored_button(label(kIconPlay, "Gerar partes..."), {-1.0F, 42.0F}, kGreen)) {
            const auto directory = sto::platform::select_directory(L"Selecione a pasta de destino");
            if (!directory.empty()) {
                module.pdf_range_output_directory = directory;
                module.begin([&module] { run_pdf_range_split(module); });
            }
        }
    }
    render_progress(module);
}

void move_merge_pdf(State& module, std::size_t from, std::size_t to, bool insert_after) {
    if (from >= module.inputs.size() || to >= module.inputs.size() || from == to) return;
    if (insert_after) ++to;
    if (from < to) --to;
    auto file = std::move(module.inputs[from]);
    const int pages = from < module.merge_page_counts.size() ? module.merge_page_counts[from] : 0;
    module.inputs.erase(module.inputs.begin() + static_cast<std::ptrdiff_t>(from));
    if (from < module.merge_page_counts.size()) module.merge_page_counts.erase(module.merge_page_counts.begin() + static_cast<std::ptrdiff_t>(from));
    to = std::min(to, module.inputs.size());
    module.inputs.insert(module.inputs.begin() + static_cast<std::ptrdiff_t>(to), std::move(file));
    module.merge_page_counts.insert(module.merge_page_counts.begin() + static_cast<std::ptrdiff_t>(to), pages);
}

void remove_merge_pdf(State& module, std::size_t index) {
    if (index >= module.inputs.size()) return;
    module.inputs.erase(module.inputs.begin() + static_cast<std::ptrdiff_t>(index));
    if (index < module.merge_page_counts.size()) module.merge_page_counts.erase(module.merge_page_counts.begin() + static_cast<std::ptrdiff_t>(index));
}

void add_merge_pdfs(State& module, const std::vector<std::filesystem::path>& files) {
    for (const auto& file : files) {
        module.inputs.push_back(file);
        module.merge_page_counts.push_back(pdf_page_count(file));
    }
}

void render_merge_order(State& module) {
    ImGui::TextColored(kSilver, "Ordem de junção: %zu PDF(s)", module.inputs.size());
    ImGui::BeginChild("merge-order", {0.0F, 292.0F}, ImGuiChildFlags_Borders);
    if (module.inputs.empty()) {
        ImGui::TextColored(kMuted, "Nenhum PDF selecionado.");
    } else {
        int first_page = 1;
        for (std::size_t index = 0; index < module.inputs.size(); ++index) {
            const int pages = index < module.merge_page_counts.size() ? module.merge_page_counts[index] : 0;
            const int last_page = pages > 0 ? first_page + pages - 1 : first_page;
            ImGui::PushID(static_cast<int>(index));
            constexpr float row_height = 46.0F;
            constexpr float remove_button_size = 30.0F;
            constexpr float remove_area_width = 52.0F;
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            const float row_width = ImGui::GetContentRegionAvail().x;
            const ImVec2 row_max = {row_min.x + row_width, row_min.y + row_height};
            ImGui::InvisibleButton("merge-row", {std::max(1.0F, row_width - remove_area_width), row_height});
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 min = row_min;
            const ImVec2 max = row_max;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? ImVec4{0.13F, 0.14F, 0.15F, 1.0F} : ImVec4{0.08F, 0.09F, 0.10F, 1.0F}), 6.0F);
            draw->AddText({min.x + 10.0F, min.y + 7.0F}, ImGui::GetColorU32(kBlue), kIconPdf);
            const std::string page_text = pages > 0
                ? std::format("{} página(s)  ->  saída: {} a {}", pages, first_page, last_page)
                : "páginas não identificadas";
            draw->PushClipRect({min.x + 36.0F, min.y}, {max.x - 52.0F, max.y}, true);
            draw->AddText({min.x + 36.0F, min.y + 6.0F}, ImGui::GetColorU32(kWhite), narrow(module.inputs[index].filename().wstring()).c_str());
            draw->AddText({min.x + 36.0F, min.y + 24.0F}, ImGui::GetColorU32(kMuted), page_text.c_str());
            draw->PopClipRect();

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                const int payload = static_cast<int>(index);
                ImGui::SetDragDropPayload(kMergePdfPayload, &payload, sizeof(payload));
                ImGui::TextColored(kWhite, "%s", narrow(module.inputs[index].filename().wstring()).c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* preview = ImGui::AcceptDragDropPayload(kMergePdfPayload, ImGuiDragDropFlags_AcceptPeekOnly);
                const bool insert_after = ImGui::GetMousePos().y > (min.y + max.y) * 0.5F;
                if (preview != nullptr) {
                    const float y = insert_after ? max.y + 2.0F : min.y - 2.0F;
                    draw->AddRectFilled({min.x + 6.0F, y - 2.0F}, {max.x - 6.0F, y + 2.0F}, ImGui::GetColorU32(kBlue), 2.0F);
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMergePdfPayload)) {
                    const int from = *static_cast<const int*>(payload->Data);
                    move_merge_pdf(module, static_cast<std::size_t>(from), index, insert_after);
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::SetCursorScreenPos({max.x - remove_button_size - 10.0F, min.y + (max.y - min.y - remove_button_size) * 0.5F});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1.0F, 1.0F, 1.0F, 0.06F});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{1.0F, 1.0F, 1.0F, 0.10F});
            ImGui::PushStyleColor(ImGuiCol_Text, kError);
            if (!module.busy() && ImGui::Button(kIconTrash, {remove_button_size, remove_button_size})) {
                remove_merge_pdf(module, index);
                ImGui::PopStyleColor(4);
                ImGui::PopID();
                break;
            }
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remover PDF da lista");
            if (pages > 0) first_page = last_page + 1;
            ImGui::SetCursorScreenPos({min.x, max.y});
            ImGui::PopID();
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();
}

void render_merge_pdf(State& module) {
    if (!module.busy() && colored_button(label(kIconBack, "Voltar"), {112.0F, 34.0F}, ImVec4{0.14F, 0.15F, 0.16F, 1.0F})) module.tool = Tool::dashboard;
    ImGui::Spacing();
    ImGui::BeginChild("merge-pdf-header", {0.0F, 116.0F}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    accent_line(52.0F, kPurple);
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "Juntar PDFs");
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::TextColored(kMuted, "Monte a ordem dos arquivos, confira a sequência de páginas e gere um único PDF.");
    ImGui::EndChild();
    ImGui::Spacing();

    if (!module.busy() && colored_button(label(kIconFolder, module.inputs.empty() ? "Selecionar PDFs" : "Adicionar PDFs"), {210.0F, 38.0F}, kBlue)) {
        const auto files = sto::platform::select_files(L"Selecione os PDFs", {{L"Arquivos PDF", L"*.pdf"}});
        if (!files.empty()) {
            if (module.inputs.empty()) module.merge_page_counts.clear();
            add_merge_pdfs(module, files);
            module.status = Status::idle;
            module.progress = 0.0F;
            module.set_output_directory({});
        }
    }
    ImGui::SameLine();
    if (!module.busy() && !module.inputs.empty() && colored_button("Limpar lista", {130.0F, 38.0F}, kRed)) {
        module.inputs.clear();
        module.merge_page_counts.clear();
        module.merge_pdf_output.clear();
        module.status = Status::idle;
        module.progress = 0.0F;
        module.set_output_directory({});
    }

    ImGui::Spacing();
    render_merge_order(module);

    int total_pages = 0;
    bool pages_valid = module.inputs.size() >= 2;
    for (const int pages : module.merge_page_counts) {
        pages_valid = pages_valid && pages > 0;
        total_pages += std::max(0, pages);
    }
    ImGui::Spacing();
    ImGui::TextColored(pages_valid ? kMuted : kError, "Prévia final: %zu arquivo(s), %d página(s).", module.inputs.size(), total_pages);
    ImGui::Spacing();
    if (!module.busy() && pages_valid && colored_button(label(kIconPlay, "Juntar e salvar como..."), {-1.0F, 42.0F}, kGreen)) {
        const auto output = sto::platform::select_save_file(
            L"Salvar PDF juntado como",
            {{L"Arquivo PDF", L"*.pdf"}},
            L"pdf_juntado.pdf"
        );
        if (!output.empty()) {
            module.merge_pdf_output = ensure_extension(output, L".pdf");
            module.begin([&module] { run_pdf_merge(module); });
        }
    }
    render_progress(module);
}

void render_pdf_to_word(State& module) {
    if (!module.busy() && colored_button(label(kIconBack, "Voltar"), {112.0F, 34.0F}, ImVec4{0.14F, 0.15F, 0.16F, 1.0F})) module.tool = Tool::dashboard;
    ImGui::Spacing();
    ImGui::BeginChild("pdf-to-word-header", {0.0F, 116.0F}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    accent_line(52.0F, kBlue);
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "PDF para Word");
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::TextColored(kMuted, "Converta um PDF digital em um arquivo DOCX editável, escolhendo exatamente onde salvar.");
    ImGui::EndChild();
    ImGui::Spacing();

    ImGui::BeginChild("pdf-to-word-options", {0.0F, 70.0F}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextColored(kMuted, "Versão inicial sem OCR. PDFs digitalizados serão suportados em uma próxima etapa com detecção inteligente.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::Spacing();

    if (!module.busy() && colored_button(label(kIconFolder, "Selecionar PDF"), {190.0F, 38.0F}, kBlue)) {
        module.inputs = sto::platform::select_files(L"Selecione o PDF", {{L"Arquivos PDF", L"*.pdf"}});
        if (module.inputs.size() > 1) module.inputs.resize(1);
        module.pdf_to_word_output.clear();
        module.status = Status::idle;
        module.progress = 0.0F;
        module.set_output_directory({});
    }

    ImGui::Spacing();
    render_selected_files(module);
    ImGui::Spacing();

    if (!module.busy() && !module.inputs.empty() && colored_button(label(kIconPlay, "Converter e salvar como..."), {-1.0F, 42.0F}, kGreen)) {
        const auto& input = module.inputs.front();
        const auto default_name = sto::files::sanitize_stem(input.stem().wstring()) + L".docx";
        const auto output = sto::platform::select_save_file(
            L"Salvar Word como",
            {{L"Documento Word", L"*.docx"}},
            default_name
        );
        if (!output.empty()) {
            module.pdf_to_word_output = ensure_extension(output, L".docx");
            module.begin([&module] { run_pdf_to_word(module); });
        }
    }
    render_progress(module);
}

void render_filenames(State& module) {
    if (!module.busy() && colored_button(label(kIconBack, "Voltar"), {112.0F, 34.0F}, ImVec4{0.14F, 0.15F, 0.16F, 1.0F})) module.tool = Tool::dashboard;
    ImGui::Spacing();
    ImGui::BeginChild("filenames-header", {0.0F, 116.0F}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    accent_line(52.0F, kSilver);
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    ImGui::TextColored(kWhite, "Correção de Nomes");
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::TextColored(kMuted, "Visualize as alterações antes de renomear arquivos em lote.");
    ImGui::EndChild();
    ImGui::Spacing();
    if (!module.busy() && colored_button(label(kIconFolder, "Selecionar pasta"), {210.0F, 38.0F}, kBlue)) {
        const auto directory = sto::platform::select_directory(L"Selecione a pasta com os arquivos");
        if (!directory.empty()) {
            module.set_output_directory(directory);
            module.status = Status::idle;
            module.progress = 0.0F;
            build_rename_preview(module, directory);
        }
    }
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Alterações previstas: %zu", module.rename_preview.size());
    ImGui::BeginChild("rename-preview", {0.0F, 280.0F}, ImGuiChildFlags_Borders);
    for (const auto& item : module.rename_preview) ImGui::TextWrapped("%s  ->  %s", narrow(item.source.filename().wstring()).c_str(), narrow(item.destination.filename().wstring()).c_str());
    ImGui::EndChild();
    ImGui::Spacing();
    if (!module.busy() && !module.rename_preview.empty() && colored_button(label(kIconPlay, "Aplicar alterações"), {-1.0F, 42.0F}, kGreen)) module.begin([&module] { run_rename(module); });
    render_progress(module);
}
}

void render() {
    State& module = state();
    constexpr float horizontal_margin = 10.0F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + horizontal_margin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0F, 8.0F});
    ImGui::BeginChild(
        "utilities-module-frame",
        {ImGui::GetContentRegionAvail().x - horizontal_margin, 0.0F},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    ImGui::TextColored(kSilver, "MÓDULOS  /  FERRAMENTAS DIVERSAS");
    ImGui::Spacing();
    switch (module.tool) {
    case Tool::dashboard: render_dashboard(module); break;
    case Tool::codec: render_standard_tool(module, "Correção de Codec de Vídeos", "Converta vídeos para MP4 H.264/AAC com compatibilidade ampliada.", {{L"Arquivos de vídeo", L"*.mp4;*.avi;*.mkv;*.mov;*.webm"}, {L"Todos os arquivos", L"*.*"}}, run_codec_batch); break;
    case Tool::audio: render_audio(module); break;
    case Tool::compress_pdf: render_compress_pdf(module); break;
    case Tool::split_pdf: render_split_pdf(module); break;
    case Tool::range_split_pdf: render_range_split_pdf(module); break;
    case Tool::merge_pdf: render_merge_pdf(module); break;
    case Tool::pdf_to_word: render_pdf_to_word(module); break;
    case Tool::filenames: render_filenames(module); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}
}
