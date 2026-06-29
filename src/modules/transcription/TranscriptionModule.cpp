#include "modules/transcription/TranscriptionModule.hpp"

#include "core/database/TranscriptionRepository.hpp"
#include "core/files/FileUtils.hpp"
#include "core/platform/FileDialogs.hpp"
#include "core/text/PortugueseDate.hpp"
#include "core/platform/ProcessRunner.hpp"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ui/TextWrap.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>
#include <windows.h>

namespace sto::modules::transcription {
namespace {
constexpr const char* kIconFolder = "\xef\x81\xbb";
constexpr const char* kIconMicrophone = "\xef\x84\xb0";
constexpr const char* kIconPlay = "\xef\x81\x8b";
constexpr const char* kIconCopy = "\xef\x83\x85";
constexpr const char* kIconSave = "\xef\x83\x87";
constexpr const char* kIconGear = "\xef\x80\x93";
constexpr const char* kIconTrash = "\xef\x87\xb8";
constexpr const char* kIconCancel = "\xef\x81\x8d";
constexpr const char* kIconChevronRight = "\xef\x81\x94";
constexpr const char* kIconChevronLeft = "\xef\x81\x93";
constexpr const char* kIconChevronDown = "\xef\x81\xb8";
constexpr const char* kIconPlus = "\xef\x81\xa7";
constexpr const char* kIconMedia = "\xef\x88\xb4";
constexpr const char* kIconClock = "\xef\x80\x97";
constexpr const char* kIconPause = "\xef\x81\x8c";
constexpr const char* kIconCheck = "\xef\x80\x8c";
constexpr const char* kIconCalendar = "\xef\x84\xb3";
constexpr ImVec4 kWhite = {0.94F, 0.95F, 0.96F, 1.00F};
constexpr ImVec4 kSilver = {0.70F, 0.73F, 0.76F, 1.00F};
constexpr ImVec4 kMuted = {0.52F, 0.55F, 0.58F, 1.00F};
constexpr ImVec4 kSuccess = {0.20F, 1.00F, 0.36F, 1.00F};
constexpr ImVec4 kError = {1.00F, 0.22F, 0.22F, 1.00F};
constexpr ImVec4 kBlue = {0.00F, 0.48F, 1.00F, 1.00F};
constexpr ImVec4 kGreen = {0.16F, 0.65F, 0.27F, 1.00F};
constexpr ImVec4 kPurple = {0.44F, 0.26F, 0.76F, 1.00F};
constexpr ImVec4 kRed = {0.86F, 0.21F, 0.27F, 1.00F};
constexpr ImVec4 kYellow = {1.00F, 0.76F, 0.03F, 1.00F};
constexpr ImVec4 kDarkText = {0.05F, 0.06F, 0.07F, 1.00F};

constexpr std::array kHearingTypes = {
    "Comunicação de Ocorrência",
    "Declaração",
    "Depoimento",
    "Interrogatório",
};
constexpr std::array kProcedures = {
    "Procedimento Tradicional",
    "Procedimento Formal",
};

enum class Status { idle, queued, running, paused, completed, failed, cancelled };
enum class NoteKind { success, warning, error };

std::filesystem::path runtime(const wchar_t* relative) {
    return sto::files::executable_directory() / relative;
}

std::filesystem::path database_path() {
    return sto::files::executable_directory() / "data" / "sto.db";
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, output.data(), size, nullptr, nullptr);
    output.resize(static_cast<std::size_t>(size - 1));
    return output;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, output.data(), size);
    output.resize(static_cast<std::size_t>(size - 1));
    return output;
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

std::string label(const char* icon, const char* text) {
    return std::string(icon) + "  " + text;
}

// ---- Echo / repetition cleaning ----

static std::string echo_normalize(const std::string& s) {
    std::string result;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!result.empty() && result.back() != ' ') result += ' ';
        } else {
            result += static_cast<char>(std::tolower(c));
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

// Removes runs of 3+ identical word-sequences from a single line of text.
static std::string clean_word_echo(const std::string& line) {
    std::vector<std::string> words;
    {
        std::istringstream ss(line);
        std::string w;
        while (ss >> w) words.push_back(w);
    }
    if (words.size() < 9) return line;

    auto key = [](const std::string& s) {
        std::string r;
        for (unsigned char c : s)
            if (std::isalnum(c) || c > 127) r += static_cast<char>(std::tolower(c));
        return r;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        const std::size_t n = words.size();
        if (n < 9) break;
        for (std::size_t start = 0; start < n && !changed; ++start) {
            const std::size_t max_pattern = std::min<std::size_t>(50, (n - start) / 3);
            for (std::size_t len = 2; len <= max_pattern && !changed; ++len) {
                if (start + len * 3 > n) break;
                if (key(words[start]) != key(words[start + len])) continue;
                bool match = true;
                for (std::size_t k = 0; k < len && match; ++k)
                    if (key(words[start + k]) != key(words[start + len + k])) match = false;
                if (!match) continue;
                std::size_t count = 2;
                while (start + (count + 1) * len <= n) {
                    bool more = true;
                    for (std::size_t k = 0; k < len && more; ++k)
                        if (key(words[start + k]) != key(words[start + count * len + k])) more = false;
                    if (!more) break;
                    ++count;
                }
                if (count < 3) continue;
                std::vector<std::string> deduped;
                deduped.reserve(n - (count - 1) * len);
                deduped.insert(deduped.end(), words.begin(), words.begin() + static_cast<std::ptrdiff_t>(start + len));
                deduped.insert(deduped.end(), words.begin() + static_cast<std::ptrdiff_t>(start + count * len), words.end());
                words = std::move(deduped);
                changed = true;
            }
        }
    }

    std::string result;
    result.reserve(line.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) result += ' ';
        result += words[i];
    }
    return result;
}

// Echo detection result: clean text for copying and annotated text for display.
// In annotated, echo segments are wrapped in \x01 toggle markers so the
// renderer can colour them red without affecting what gets copied.
struct EchoResult { std::string clean; std::string annotated; };

// Detects echo-style repetitions introduced by poor audio quality and returns
// both a clean version (repeats removed) and an annotated version (repeats
// enclosed in \x01 markers for red highlighting in the display).
// Pass 1: sentence-level (same sentence repeated 3+ times consecutively).
// Pass 2: word-level (same word-sequence repeated 3+ times within a line).
static EchoResult process_echo_repetitions(const std::string& raw) {
    if (raw.size() < 30) return {raw, raw};

    struct Seg { std::string text; char tail; };
    std::vector<Seg> segs;
    {
        std::string cur;
        for (std::size_t i = 0; i < raw.size(); ) {
            const char c = raw[i++];
            cur += c;
            const bool sent = (c == '.' || c == '!' || c == '?');
            const bool nl   = (c == '\n');
            if (sent) {
                char tail = '\0';
                if (i < raw.size() && (raw[i] == ' ' || raw[i] == '\n')) tail = raw[i++];
                segs.push_back({std::move(cur), tail});
                cur.clear();
            } else if (nl) {
                segs.push_back({std::move(cur), '\0'});
                cur.clear();
            }
        }
        if (!cur.empty()) segs.push_back({std::move(cur), '\0'});
    }

    std::string clean_pass1;
    std::string annotated;
    clean_pass1.reserve(raw.size());
    annotated.reserve(raw.size());

    for (std::size_t i = 0; i < segs.size(); ) {
        const std::string k = echo_normalize(segs[i].text);
        std::size_t j = i + 1;
        if (k.size() >= 15) {
            while (j < segs.size() && echo_normalize(segs[j].text) == k) ++j;
        }
        const std::size_t cnt = j - i;
        if (cnt >= 3 && k.size() >= 15) {
            // First occurrence goes to both; repeats go to annotated only, marked as echo.
            clean_pass1 += segs[i].text;
            if (segs[i].tail != '\0') clean_pass1 += segs[i].tail;
            annotated += segs[i].text;
            if (segs[i].tail != '\0') annotated += segs[i].tail;
            annotated += '\x01';
            for (std::size_t s = i + 1; s < j; ++s) {
                annotated += segs[s].text;
                if (segs[s].tail != '\0') annotated += segs[s].tail;
            }
            annotated += '\x01';
        } else {
            for (std::size_t s = i; s < j; ++s) {
                clean_pass1 += segs[s].text;
                if (segs[s].tail != '\0') clean_pass1 += segs[s].tail;
                annotated += segs[s].text;
                if (segs[s].tail != '\0') annotated += segs[s].tail;
            }
        }
        i = j;
    }

    // Word-level pass on clean text.
    std::string clean;
    clean.reserve(clean_pass1.size());
    {
        std::istringstream stream(clean_pass1);
        std::string line;
        bool first = true;
        while (std::getline(stream, line)) {
            if (!first) clean += '\n';
            first = false;
            clean += clean_word_echo(line);
        }
        if (!clean_pass1.empty() && clean_pass1.back() == '\n') clean += '\n';
    }

    // Word-level pass on non-echo segments of annotated so display and copy
    // are consistent for the non-echo parts.
    std::string annotated_final;
    annotated_final.reserve(annotated.size());
    {
        bool in_echo = false;
        std::string seg;
        const auto flush_seg = [&]() {
            if (in_echo) {
                annotated_final += seg;
            } else {
                std::istringstream stream(seg);
                std::string line;
                bool first = true;
                while (std::getline(stream, line)) {
                    if (!first) annotated_final += '\n';
                    first = false;
                    annotated_final += clean_word_echo(line);
                }
                if (!seg.empty() && seg.back() == '\n') annotated_final += '\n';
            }
            seg.clear();
        };
        for (const char c : annotated) {
            if (c == '\x01') {
                flush_seg();
                annotated_final += '\x01';
                in_echo = !in_echo;
            } else {
                seg += c;
            }
        }
        flush_seg();
    }

    return {clean, annotated_final};
}

// ---- Filename metadata detection ----

struct FilenameDetection { int hearing_type = -1; std::string party_name; std::string date; };

static FilenameDetection detect_from_filename(const std::filesystem::path& file) {
    FilenameDetection det;
    const std::wstring stem = file.stem().wstring();

    std::wstring lower = stem;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));

    // Detect hearing type keyword and record its end position for the fallback path.
    std::wstring matched_kw_lower;
    std::size_t keyword_end = 0;
    for (int i = 0; i < static_cast<int>(kHearingTypes.size()); ++i) {
        std::wstring lkw = widen(kHearingTypes[i]);
        for (auto& c : lkw) c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
        const auto pos = lower.find(lkw);
        if (pos == std::wstring::npos) continue;
        det.hearing_type = i;
        matched_kw_lower = lkw;
        keyword_end = pos + lkw.size();
        break;
    }

    // Returns true if the token looks like a date, timestamp, file version, or
    // pure number — anything that is definitely NOT a person's name.
    // Examples that return true: "11.11.2025", "2025-01-15", "15/03/2024", "123", "-", "v2".
    const auto is_junk = [](const std::wstring& s) -> bool {
        if (s.empty()) return true;
        int digits = 0, letters = 0, date_seps = 0;
        for (wchar_t c : s) {
            if (std::iswdigit(static_cast<wint_t>(c)))      ++digits;
            else if (std::iswalpha(static_cast<wint_t>(c))) ++letters;
            else if (c == L'.' || c == L'/' || c == L'-')   ++date_seps;
        }
        if (letters == 0) return true; // pure digits / separators
        // Digits outnumber letters AND the token is dense with digits+separators → date/code
        return digits > letters && (digits + date_seps) * 2 >= static_cast<int>(s.size());
    };

    // Scores a candidate segment as a person's name. Returns -1 if disqualified.
    // Higher score = more likely a real name.
    const auto name_score = [&](const std::wstring& s) -> int {
        if (is_junk(s)) return -1;
        int letters = 0, uppers = 0, words = 1;
        bool prev_space = false;
        for (wchar_t c : s) {
            if (std::iswalpha(static_cast<wint_t>(c))) {
                ++letters;
                if (std::iswupper(static_cast<wint_t>(c))) ++uppers;
                prev_space = false;
            } else if (c == L' ') {
                if (!prev_space) ++words;
                prev_space = true;
            }
        }
        if (letters < 2) return -1;
        int score = letters * 2 + words * 5;
        // Mostly/all uppercase is the standard format for names in Brazilian police docs
        if (uppers * 10 >= letters * 7) score += 10;
        return score;
    };

    // Split the stem into segments on " - " and score each one.
    const std::wstring sep = L" - ";
    std::vector<std::wstring> segments;
    for (std::size_t pos = 0, f = 0; pos <= stem.size(); pos = f + sep.size()) {
        f = stem.find(sep, pos);
        if (f == std::wstring::npos) { segments.push_back(stem.substr(pos)); break; }
        segments.push_back(stem.substr(pos, f - pos));
    }

    int best_score = -1;
    std::wstring best_name;
    for (const auto& seg : segments) {
        // Skip the hearing type keyword segment itself, including extended forms
        // like "Depoimento Especial" when the matched keyword is "Depoimento".
        if (!matched_kw_lower.empty()) {
            std::wstring lseg = seg;
            for (auto& c : lseg) c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
            const bool is_kw_prefix = lseg.size() >= matched_kw_lower.size()
                && lseg.substr(0, matched_kw_lower.size()) == matched_kw_lower
                && (lseg.size() == matched_kw_lower.size() || lseg[matched_kw_lower.size()] == L' ');
            if (is_kw_prefix) continue;
        }
        const int sc = name_score(seg);
        if (sc > best_score) { best_score = sc; best_name = seg; }
    }

    // Fallback for filenames without " - " separators (e.g. "Depoimento JOAO SILVA 11.11.2025"):
    // scan word-by-word after the keyword and stop at the first date/number token.
    if (best_score <= 0 && det.hearing_type >= 0 && keyword_end < stem.size()) {
        std::wstring tail = stem.substr(keyword_end);
        std::size_t start = 0;
        while (start < tail.size() && (std::iswspace(static_cast<wint_t>(tail[start]))
               || tail[start] == L'-' || tail[start] == L':' || tail[start] == L'_'))
            ++start;
        tail = tail.substr(start);

        std::wistringstream ws(tail);
        std::wstring word, acc;
        while (ws >> word) {
            if (is_junk(word)) break;
            if (!acc.empty()) acc += L' ';
            acc += word;
        }
        if (name_score(acc) > 0) best_name = acc;
    }

    while (!best_name.empty() && std::iswspace(static_cast<wint_t>(best_name.front())))
        best_name.erase(best_name.begin());
    while (!best_name.empty() && std::iswspace(static_cast<wint_t>(best_name.back())))
        best_name.pop_back();

    if (!best_name.empty()) det.party_name = narrow(best_name);

    // Detect date pattern DD.MM.YYYY or DD-MM-YYYY anywhere in the stem.
    for (std::size_t i = 0; i + 9 < stem.size(); ++i) {
        if (!std::iswdigit(static_cast<wint_t>(stem[i])) || !std::iswdigit(static_cast<wint_t>(stem[i + 1]))) continue;
        const wchar_t date_sep = stem[i + 2];
        if (date_sep != L'.' && date_sep != L'-') continue;
        if (!std::iswdigit(static_cast<wint_t>(stem[i + 3])) || !std::iswdigit(static_cast<wint_t>(stem[i + 4]))) continue;
        if (stem[i + 5] != date_sep) continue;
        if (!std::iswdigit(static_cast<wint_t>(stem[i + 6])) || !std::iswdigit(static_cast<wint_t>(stem[i + 7]))
            || !std::iswdigit(static_cast<wint_t>(stem[i + 8])) || !std::iswdigit(static_cast<wint_t>(stem[i + 9]))) continue;
        if (i > 0 && std::iswdigit(static_cast<wint_t>(stem[i - 1]))) continue;
        if (i + 10 < stem.size() && std::iswdigit(static_cast<wint_t>(stem[i + 10]))) continue;
        const int d = (stem[i] - L'0') * 10 + (stem[i + 1] - L'0');
        const int m = (stem[i + 3] - L'0') * 10 + (stem[i + 4] - L'0');
        const int y = (stem[i + 6] - L'0') * 1000 + (stem[i + 7] - L'0') * 100
                    + (stem[i + 8] - L'0') * 10 + (stem[i + 9] - L'0');
        if (d < 1 || d > 31 || m < 1 || m > 12 || y < 2000 || y > 2099) continue;
        char buf[11];
        std::snprintf(buf, sizeof(buf), "%02d/%02d/%04d", d, m, y);
        det.date = buf;
        break;
    }

    return det;
}

using sto::ui::wrap_text;

bool colored_button(const std::string& text, const ImVec2& size, const ImVec4& color, const ImVec4& text_color = kWhite) {
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {std::min(color.x + 0.10F, 1.0F), std::min(color.y + 0.10F, 1.0F), std::min(color.z + 0.10F, 1.0F), 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {color.x * 0.78F, color.y * 0.78F, color.z * 0.78F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    const bool clicked = ImGui::Button(text.c_str(), size);
    ImGui::PopStyleColor(4);
    return clicked;
}

struct NotificationContent {
    ULONGLONG started = 0;
    std::wstring title;
    std::wstring message;
};

LRESULT CALLBACK notification_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    auto* content = reinterpret_cast<NotificationContent*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_TIMER:
        if (wparam == 1) {
            DestroyWindow(window);
            return 0;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        DestroyWindow(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC window_device = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HDC device = CreateCompatibleDC(window_device);
        HBITMAP bitmap = CreateCompatibleBitmap(window_device, client.right, client.bottom);
        HGDIOBJ previous_bitmap = SelectObject(device, bitmap);
        HBRUSH background = CreateSolidBrush(RGB(30, 33, 36));
        FillRect(device, &client, background);
        DeleteObject(background);
        HPEN border = CreatePen(PS_SOLID, 3, RGB(40, 167, 69));
        HGDIOBJ previous_pen = SelectObject(device, border);
        HGDIOBJ previous_brush = SelectObject(device, GetStockObject(HOLLOW_BRUSH));
        Rectangle(device, 1, 1, client.right - 1, client.bottom - 1);
        SelectObject(device, previous_brush);
        SelectObject(device, previous_pen);
        DeleteObject(border);

        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, RGB(40, 210, 82));
        HFONT title_font = CreateFontW(24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ previous_font = SelectObject(device, title_font);
        RECT title{28, 20, client.right - 28, 54};
        DrawTextW(device, content != nullptr ? content->title.c_str() : L"STO", -1, &title, DT_CENTER | DT_SINGLELINE);
        SelectObject(device, previous_font);
        DeleteObject(title_font);

        SetTextColor(device, RGB(238, 240, 242));
        HFONT text_font = CreateFontW(19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        previous_font = SelectObject(device, text_font);
        RECT body{34, 66, client.right - 34, client.bottom - 24};
        DrawTextW(device, content != nullptr ? content->message.c_str() : L"Operação concluída com sucesso.", -1, &body, DT_CENTER | DT_WORDBREAK);
        SelectObject(device, previous_font);
        DeleteObject(text_font);

        const ULONGLONG started = content != nullptr ? content->started : GetTickCount64();
        const float elapsed = static_cast<float>(GetTickCount64() - started);
        const int progress_width = static_cast<int>((client.right - 48) * std::clamp(1.0F - (elapsed / 5000.0F), 0.0F, 1.0F));
        HBRUSH track = CreateSolidBrush(RGB(58, 62, 66));
        RECT track_rect{24, client.bottom - 14, client.right - 24, client.bottom - 8};
        FillRect(device, &track_rect, track);
        DeleteObject(track);
        HBRUSH progress = CreateSolidBrush(RGB(40, 167, 69));
        RECT progress_rect{24, client.bottom - 14, 24 + progress_width, client.bottom - 8};
        FillRect(device, &progress_rect, progress);
        DeleteObject(progress);
        BitBlt(window_device, 0, 0, client.right, client.bottom, device, 0, 0, SRCCOPY);
        SelectObject(device, previous_bitmap);
        DeleteObject(bitmap);
        DeleteDC(device);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        delete content;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

struct NotifThreadData {
    wchar_t title[512];
    wchar_t message[1024];
};

static DWORD WINAPI notif_thread_proc(LPVOID pv) {
    auto* data = static_cast<NotifThreadData*>(pv);
    wchar_t title[512];
    wchar_t message[1024];
    wcsncpy_s(title, _countof(title), data->title, _TRUNCATE);
    wcsncpy_s(message, _countof(message), data->message, _TRUNCATE);
    delete data;

    constexpr wchar_t class_name[] = L"STOCompletionNotification";
    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.lpfnWndProc   = notification_window_procedure;
    window_class.hInstance     = GetModuleHandleW(nullptr);
    window_class.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = class_name;
    RegisterClassExW(&window_class); // idempotent: ERROR_CLASS_ALREADY_EXISTS is ignored

    constexpr int width = 520;
    constexpr int height = 158;
    MONITORINFO monitor_info{sizeof(monitor_info)};
    GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &monitor_info);
    const RECT area = monitor_info.rcWork;
    const int x = area.left + ((area.right - area.left - width) / 2);
    const int y = area.top + ((area.bottom - area.top - height) / 2);
    auto* content = new NotificationContent{GetTickCount64(), title, message};
    HWND window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        class_name,
        L"STO",
        WS_POPUP,
        x, y, width, height,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        content
    );
    if (window == nullptr) {
        delete content;
        return 0;
    }
    SetLayeredWindowAttributes(window, 0, 232, LWA_ALPHA);
    SetWindowRgn(window, CreateRoundRectRgn(0, 0, width + 1, height + 1, 18, 18), TRUE);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetWindowPos(window, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetTimer(window, 1, 5000, nullptr);
    SetTimer(window, 2, 30, nullptr);
    MSG window_message{};
    while (GetMessageW(&window_message, nullptr, 0, 0) > 0) {
        TranslateMessage(&window_message);
        DispatchMessageW(&window_message);
    }
    return 0;
}

void show_windows_notification(std::wstring title, std::wstring message) {
    auto* data = new NotifThreadData{};
    wcsncpy_s(data->title, _countof(data->title), title.c_str(), _TRUNCATE);
    wcsncpy_s(data->message, _countof(data->message), message.c_str(), _TRUNCATE);
    const HANDLE t = CreateThread(nullptr, 0, notif_thread_proc, data, 0, nullptr);
    if (t != nullptr) CloseHandle(t);
    else delete data;
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
    auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
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

std::string json_prompt(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto key = json.find("\"prompt\"");
    const auto colon = key == std::string::npos ? key : json.find(':', key);
    const auto quote = colon == std::string::npos ? colon : json.find('"', colon);
    if (quote == std::string::npos) return {};
    std::string output;
    for (std::size_t index = quote + 1; index < json.size(); ++index) {
        const char character = json[index];
        if (character == '"') break;
        if (character != '\\' || index + 1 >= json.size()) {
            output += character;
            continue;
        }
        const char escaped = json[++index];
        if (escaped == 'n') output += '\n';
        else if (escaped == 'r') output += '\r';
        else if (escaped == 't') output += '\t';
        else output += escaped;
    }
    return output;
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

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return file ? std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()) : std::string{};
}

std::string format_duration(double seconds) {
    const int rounded = std::max(0, static_cast<int>(std::ceil(seconds)));
    const int hours = rounded / 3600;
    const int minutes = (rounded % 3600) / 60;
    const int remaining_seconds = rounded % 60;
    if (hours > 0) return std::format("{}h {:02d}min {:02d}s", hours, minutes, remaining_seconds);
    if (minutes > 0) return std::format("{}min {:02d}s", minutes, remaining_seconds);
    return std::format("{}s", remaining_seconds);
}

std::string ellipsize(std::string text, std::size_t limit) {
    const std::wstring wide = widen(text);
    if (wide.size() <= limit) return text;
    if (limit <= 3) return narrow(wide.substr(0, limit));
    return narrow(wide.substr(0, limit - 3) + L"...");
}

struct Session {
    long long id = 0;
    std::filesystem::path input;
    std::array<char, 256> party_name{};
    std::array<char, 11> data_oitiva{};
    std::string additional_context;
    std::string transcript;
    std::string transcript_annotated;
    std::string transcript_display;
    int hearing_type = 0;
    int procedure = 0;
    bool show_context = false;
    int queue_order = 0;
    long long group_id = 0;
    float transcript_wrap_width = 0.0F;
    std::atomic<Status> status = Status::idle;
    std::atomic_bool transcribing = false;
    std::atomic_bool transcript_needs_wrap = false;
    std::atomic<float> progress = 0.0F;
    std::atomic<float> whisper_progress = 0.0F;
    std::atomic<double> estimated_seconds = 0.0;
    std::atomic<double> projected_total_seconds = 0.0;
    std::atomic<long long> transcription_started_ms = 0;
    std::mutex mutex;
    std::string message = "Selecione um arquivo de áudio ou vídeo para começar.";

    bool busy() const { return status == Status::running; }

    void set_message(std::string value) {
        std::scoped_lock lock(mutex);
        message = std::move(value);
    }

    std::string get_message() {
        std::scoped_lock lock(mutex);
        return message;
    }
};

std::string stored_status(Status status) {
    switch (status) {
    case Status::queued: return "queued";
    case Status::running: return "running";
    case Status::paused: return "paused";
    case Status::completed: return "completed";
    case Status::failed: return "failed";
    case Status::cancelled: return "cancelled";
    default: return "draft";
    }
}

Status loaded_status(const std::string& status) {
    if (status == "queued") return Status::queued;
    if (status == "running" || status == "paused") return Status::paused;
    if (status == "completed") return Status::completed;
    if (status == "failed") return Status::failed;
    if (status == "cancelled") return Status::cancelled;
    return Status::idle;
}

sto::database::TranscriptionSession stored_session(const Session& session) {
    return {
        session.id,
        narrow(session.input.wstring()),
        session.party_name.data(),
        session.additional_context,
        session.transcript,
        session.hearing_type,
        session.procedure,
        session.show_context,
        stored_status(session.status.load()),
        session.queue_order,
        session.group_id,
        session.data_oitiva.data(),
    };
}

std::shared_ptr<Session> loaded_session(const sto::database::TranscriptionSession& stored) {
    auto session = std::make_shared<Session>();
    session->id = stored.id;
    session->input = widen(stored.input_path);
    std::snprintf(session->party_name.data(), session->party_name.size(), "%s", stored.party_name.c_str());
    std::snprintf(session->data_oitiva.data(), session->data_oitiva.size(), "%s", stored.date_oitiva.c_str());
    session->additional_context = stored.additional_context;
    if (!stored.transcript.empty()) {
        const auto echo_result = process_echo_repetitions(stored.transcript);
        session->transcript = echo_result.clean;
        session->transcript_annotated = echo_result.annotated;
    }
    session->hearing_type = stored.hearing_type;
    session->procedure = stored.procedure;
    session->show_context = stored.show_context;
    session->queue_order = stored.queue_order;
    session->group_id = stored.group_id;
    session->status = loaded_status(stored.status);
    session->transcript_needs_wrap = true;
    return session;
}

struct Group {
    long long id = 0;
    std::array<char, 256> name{};
    int queue_order = 0;
    bool collapsed = false;
};

sto::database::TranscriptionGroup stored_group(const Group& group) {
    return {group.id, group.name.data(), group.queue_order, group.collapsed};
}

std::shared_ptr<Group> loaded_group(const sto::database::TranscriptionGroup& stored) {
    auto group = std::make_shared<Group>();
    group->id = stored.id;
    std::snprintf(group->name.data(), group->name.size(), "%s", stored.name.c_str());
    group->queue_order = stored.queue_order;
    group->collapsed = stored.collapsed;
    return group;
}

// A "container" is either the top-level queue (id == 0, holding groups and
// ungrouped sessions in one shared order) or a group's children (id == group id,
// holding only that group's sessions in their own order space).
struct QueueKey {
    bool is_group = false;
    long long id = 0;
    bool operator==(const QueueKey&) const = default;
};

struct State {
    sto::database::TranscriptionRepository repository;
    sto::database::TranscriptionPrompts prompts;
    std::string editor_context;
    std::string editor_traditional;
    std::string editor_formal;
    int editor_procedure = 0;
    bool cancel_confirmation = false;
    bool retranscribe_confirmation = false;
    bool clear_confirmation = false;
    bool pause_running_confirmation = false;
    bool requeue_overwrite_confirmation = false;
    bool delete_group_confirmation = false;
    long long delete_group_target_id = 0;
    long long group_rename_target_id = 0;
    bool group_rename_focus_pending = false;
    bool queue_sidebar_collapsed = false;
    bool initialized = false;
    bool database_ready = false;
    std::atomic_bool cancel_requested = false;
    std::atomic_bool pause_after_cancel = false;
    std::atomic_bool worker_running = false;
    std::string processor_model = detect_processor_model();
    std::jthread worker;
    std::mutex sessions_mutex;
    std::vector<std::shared_ptr<Session>> sessions;
    std::vector<std::shared_ptr<Group>> groups;
    std::shared_ptr<Session> selected;
    std::vector<long long> queue_selection;
    long long selection_anchor_id = 0;
    long long pending_selection_click_id = 0;
    bool bulk_delete_confirmation = false;
    std::mutex mutex;
    std::string status_note;
    NoteKind status_note_kind = NoteKind::success;
    std::chrono::steady_clock::time_point status_note_started{};
    std::chrono::year_month calendar_view_oitiva{std::chrono::year{2024}, std::chrono::month{1}};

    ~State() {
        cancel_requested = true;
        if (worker.joinable()) worker.join();
    }

    void initialize() {
        if (initialized) return;
        initialized = true;
        try {
            const auto assets = sto::files::executable_directory() / "assets" / "transcription-prompts";
            const sto::database::TranscriptionPrompts defaults{
                json_prompt(assets / "prompt_contexto.json"),
                json_prompt(assets / "prompt_procedimento_tradicional.json"),
                json_prompt(assets / "prompt_procedimento_formal.json"),
            };
            std::string error;
            database_ready = repository.initialize(database_path(), defaults, error);
            if (!database_ready || !repository.load_prompts(prompts, error)) {
                database_ready = false;
                set_note("Erro ao inicializar configurações: " + error, NoteKind::error);
                return;
            }
            std::vector<sto::database::TranscriptionSession> stored_sessions;
            if (!repository.load_sessions(stored_sessions, error)) {
                database_ready = false;
                set_note("Erro ao carregar histórico de transcrições: " + error, NoteKind::error);
                return;
            }
            for (const auto& stored : stored_sessions) sessions.push_back(loaded_session(stored));
            std::sort(sessions.begin(), sessions.end(), [](const auto& left, const auto& right) {
                return left->queue_order < right->queue_order;
            });
            std::vector<sto::database::TranscriptionGroup> stored_groups;
            if (!repository.load_groups(stored_groups, error)) {
                database_ready = false;
                set_note("Erro ao carregar separadores da fila: " + error, NoteKind::error);
                return;
            }
            for (const auto& stored : stored_groups) groups.push_back(loaded_group(stored));
            std::sort(groups.begin(), groups.end(), [](const auto& left, const auto& right) {
                return left->queue_order < right->queue_order;
            });
            if (sessions.empty()) create_session();
            else set_single_selection(sessions.front());
            copy_prompts_to_editor();
            const auto today_init = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
            const std::chrono::year_month_day ymd_init{today_init};
            calendar_view_oitiva = ymd_init.year() / ymd_init.month();
        } catch (const std::exception& ex) {
            database_ready = false;
            set_note(std::string("Erro ao inicializar módulo: ") + ex.what(), NoteKind::error);
        } catch (...) {
            database_ready = false;
            set_note("Falha inesperada ao inicializar o módulo de transcrição.", NoteKind::error);
        }
    }

    void copy_prompts_to_editor() {
        editor_context = prompts.context;
        editor_traditional = prompts.traditional;
        editor_formal = prompts.formal;
    }

    void set_note(std::string value, NoteKind kind = NoteKind::success) {
        std::scoped_lock lock(mutex);
        status_note = value;
        status_note_kind = kind;
        status_note_started = std::chrono::steady_clock::now();
    }

    void clear_note() {
        std::scoped_lock lock(mutex);
        status_note.clear();
    }

    std::tuple<std::string, NoteKind, std::chrono::steady_clock::time_point> get_note() {
        std::scoped_lock lock(mutex);
        return {status_note, status_note_kind, status_note_started};
    }

    void create_session();
    std::shared_ptr<Session> create_session_for_file(const std::filesystem::path& file, Status status);
    void save_session(const std::shared_ptr<Session>& session);
    void create_group();
    void save_group(const std::shared_ptr<Group>& group);
    void delete_group(long long group_id);
    void move_queue_item(QueueKey moved, QueueKey target, bool insert_after);
    void move_queue_items(std::vector<QueueKey> moved, QueueKey target, bool insert_after);
    void move_to_top_level_end(long long session_id);
    std::shared_ptr<Session> next_queued_session();
    void remove_selected_session();
    void toggle_selected_queue();
    void start_queue_worker();
    void worker_loop();
    void process_session(const std::shared_ptr<Session>& session);

    // Multi-selection in the queue sidebar (Ctrl/Shift-click), used for batch
    // drag-and-drop and the batch context-menu actions on session cards.
    void set_single_selection(const std::shared_ptr<Session>& session);
    bool is_session_selected(long long id) const;
    void handle_session_click(long long session_id, bool ctrl, bool shift);
    std::vector<QueueKey> drag_payload_for(long long session_id);
    void set_selection_status(Status new_status);
    void request_pause_selection();
    void request_queue_selection();
    void confirm_pause_running();
    void remove_queue_selection();

    // Read-only helpers usable by the renderer while holding sessions_mutex.
    std::shared_ptr<Session> find_session_unlocked(long long id) const;
    std::shared_ptr<Group> find_group_unlocked(long long id) const;
    std::vector<QueueKey> container_items_unlocked(long long container_id) const;

private:
    void persist_order_locked(const std::vector<QueueKey>& items, long long container_id);
    void move_one_locked(QueueKey moved, QueueKey target, bool insert_after);
    std::vector<long long> global_session_order_unlocked() const;
};

State& state() {
    static State instance;
    instance.initialize();
    return instance;
}

void cleanup(const std::filesystem::path& wav, const std::filesystem::path& txt) {
    std::error_code error;
    std::filesystem::remove(wav, error);
    std::filesystem::remove(txt, error);
}

void State::create_session() {
    {
        std::scoped_lock lock(sessions_mutex);
        const auto draft = std::find_if(sessions.begin(), sessions.end(), [](const auto& session) {
            return session->status == Status::idle && session->input.empty() && session->transcript.empty() && session->group_id == 0;
        });
        if (draft != sessions.end()) {
            set_single_selection(*draft);
            set_note("Já existe um rascunho vazio. Use-o antes de criar outro.", NoteKind::warning);
            return;
        }
    }
    sto::database::TranscriptionSession stored;
    std::string error;
    if (!repository.create_session(stored, error)) {
        set_note("Não foi possível criar uma nova transcrição: " + error, NoteKind::error);
        return;
    }
    auto session = loaded_session(stored);
    {
        std::scoped_lock lock(sessions_mutex);
        auto items = container_items_unlocked(0);
        sessions.insert(sessions.begin(), session);
        items.insert(items.begin(), QueueKey{false, session->id});
        persist_order_locked(items, 0);
    }
    set_single_selection(session);
}

std::shared_ptr<Session> State::create_session_for_file(const std::filesystem::path& file, Status status) {
    sto::database::TranscriptionSession stored;
    std::string error;
    if (!repository.create_session(stored, error)) {
        set_note("Não foi possível criar uma transcrição para o arquivo: " + error, NoteKind::error);
        return nullptr;
    }
    auto session = loaded_session(stored);
    session->input = file;
    session->status = status;
    session->set_message(status == Status::paused
        ? "Transcrição pausada. Revise os dados e clique em Transcrever para iniciar."
        : status == Status::queued
            ? "Transcrição adicionada à fila."
            : "Arquivo selecionado. Preencha os dados e inicie a transcrição.");
    save_session(session);
    {
        std::scoped_lock lock(sessions_mutex);
        auto items = container_items_unlocked(0);
        sessions.insert(sessions.begin(), session);
        items.insert(items.begin(), QueueKey{false, session->id});
        persist_order_locked(items, 0);
    }
    return session;
}

void State::save_session(const std::shared_ptr<Session>& session) {
    if (!session) return;
    std::string error;
    if (!repository.save_session(stored_session(*session), error)) {
        set_note("Não foi possível salvar a transcrição: " + error, NoteKind::error);
    }
}

std::shared_ptr<Session> State::find_session_unlocked(long long id) const {
    for (const auto& session : sessions) {
        if (session->id == id) return session;
    }
    return nullptr;
}

std::shared_ptr<Group> State::find_group_unlocked(long long id) const {
    for (const auto& group : groups) {
        if (group->id == id) return group;
    }
    return nullptr;
}

std::vector<QueueKey> State::container_items_unlocked(long long container_id) const {
    std::vector<std::pair<QueueKey, int>> entries;
    if (container_id == 0) {
        for (const auto& group : groups) entries.emplace_back(QueueKey{true, group->id}, group->queue_order);
        for (const auto& session : sessions) {
            if (session->group_id == 0) entries.emplace_back(QueueKey{false, session->id}, session->queue_order);
        }
    } else {
        for (const auto& session : sessions) {
            if (session->group_id == container_id) entries.emplace_back(QueueKey{false, session->id}, session->queue_order);
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.second < right.second;
    });
    std::vector<QueueKey> items;
    items.reserve(entries.size());
    for (const auto& entry : entries) items.push_back(entry.first);
    return items;
}

// Flattens the whole queue (top-level sessions and every group's children, in
// display order) into a single list of session ids. Used to compute
// Shift-click ranges and to put a multi-selection back into a stable relative
// order before a batch drag-and-drop move.
std::vector<long long> State::global_session_order_unlocked() const {
    std::vector<long long> order;
    for (const auto& key : container_items_unlocked(0)) {
        if (key.is_group) {
            for (const auto& child : container_items_unlocked(key.id)) order.push_back(child.id);
        } else {
            order.push_back(key.id);
        }
    }
    return order;
}

void State::persist_order_locked(const std::vector<QueueKey>& items, long long container_id) {
    std::string error;
    const bool in_transaction = repository.begin_transaction(error);
    for (int index = 0; index < static_cast<int>(items.size()); ++index) {
        const QueueKey& key = items[static_cast<std::size_t>(index)];
        if (key.is_group) {
            auto group = find_group_unlocked(key.id);
            if (!group) continue;
            group->queue_order = index;
            if (!repository.save_group_order(group->id, index, error)) {
                set_note("Não foi possível salvar a ordem da fila: " + error, NoteKind::error);
            }
        } else {
            auto session = find_session_unlocked(key.id);
            if (!session) continue;
            session->queue_order = index;
            session->group_id = container_id;
            if (!repository.save_session_order(session->id, container_id, index, error)) {
                set_note("Não foi possível salvar a ordem da fila: " + error, NoteKind::error);
            }
        }
    }
    if (in_transaction && !repository.commit_transaction(error)) {
        set_note("Não foi possível salvar a ordem da fila: " + error, NoteKind::error);
    }
}

void State::move_queue_item(QueueKey moved, QueueKey target, bool insert_after) {
    std::scoped_lock lock(sessions_mutex);
    move_one_locked(moved, target, insert_after);
}

// Moves a block of items so they end up together at the drop target, in the
// same relative order they had in `moved`: the first item is moved to
// `target`, then each subsequent item is inserted right after the previous
// one — used when dragging a multi-selection of session cards.
void State::move_queue_items(std::vector<QueueKey> moved, QueueKey target, bool insert_after) {
    if (moved.empty()) return;
    std::scoped_lock lock(sessions_mutex);
    QueueKey current_target = target;
    bool current_insert_after = insert_after;
    for (const QueueKey& key : moved) {
        if (key == current_target) continue;
        move_one_locked(key, current_target, current_insert_after);
        current_target = key;
        current_insert_after = true;
    }
}

void State::move_one_locked(QueueKey moved, QueueKey target, bool insert_after) {
    if (moved == target) return;

    if (moved.is_group) {
        // Groups only ever live in the top-level container (0); a group can be
        // reordered relative to other groups or ungrouped sessions there, but it
        // can never be dropped inside another group.
        long long target_container = 0;
        if (!target.is_group) {
            auto target_session = find_session_unlocked(target.id);
            if (!target_session) return;
            target_container = target_session->group_id;
        }
        if (target_container != 0) return;

        auto items = container_items_unlocked(0);
        const auto moved_it = std::find(items.begin(), items.end(), moved);
        if (moved_it == items.end()) return;
        items.erase(moved_it);
        const auto target_it = std::find(items.begin(), items.end(), target);
        if (target_it == items.end()) return;
        items.insert(insert_after ? std::next(target_it) : target_it, moved);
        persist_order_locked(items, 0);
        return;
    }

    auto moved_session = find_session_unlocked(moved.id);
    if (!moved_session) return;
    const long long source_container = moved_session->group_id;

    if (target.is_group) {
        // Dropping a session onto a group header moves it into that group,
        // appended at the front of its children.
        const long long target_container = target.id;
        if (source_container == target_container) return;

        auto source_items = container_items_unlocked(source_container);
        source_items.erase(std::remove(source_items.begin(), source_items.end(), moved), source_items.end());
        persist_order_locked(source_items, source_container);

        auto target_items = container_items_unlocked(target_container);
        target_items.insert(target_items.begin(), moved);
        persist_order_locked(target_items, target_container);
        return;
    }

    auto target_session = find_session_unlocked(target.id);
    if (!target_session) return;
    const long long target_container = target_session->group_id;

    if (source_container == target_container) {
        auto items = container_items_unlocked(source_container);
        const auto moved_it = std::find(items.begin(), items.end(), moved);
        if (moved_it == items.end()) return;
        items.erase(moved_it);
        const auto target_it = std::find(items.begin(), items.end(), target);
        if (target_it == items.end()) return;
        items.insert(insert_after ? std::next(target_it) : target_it, moved);
        persist_order_locked(items, source_container);
    } else {
        auto source_items = container_items_unlocked(source_container);
        source_items.erase(std::remove(source_items.begin(), source_items.end(), moved), source_items.end());
        persist_order_locked(source_items, source_container);

        auto target_items = container_items_unlocked(target_container);
        const auto target_it = std::find(target_items.begin(), target_items.end(), target);
        if (target_it == target_items.end()) {
            target_items.push_back(moved);
        } else {
            target_items.insert(insert_after ? std::next(target_it) : target_it, moved);
        }
        persist_order_locked(target_items, target_container);
    }
}

void State::move_to_top_level_end(long long session_id) {
    std::scoped_lock lock(sessions_mutex);
    auto session = find_session_unlocked(session_id);
    if (!session || session->group_id == 0) return;

    const long long source_container = session->group_id;
    auto source_items = container_items_unlocked(source_container);
    source_items.erase(std::remove(source_items.begin(), source_items.end(), QueueKey{false, session_id}), source_items.end());
    persist_order_locked(source_items, source_container);

    auto top_items = container_items_unlocked(0);
    top_items.push_back(QueueKey{false, session_id});
    persist_order_locked(top_items, 0);
}

void State::create_group() {
    sto::database::TranscriptionGroup stored;
    stored.name = "Novo separador";
    std::string error;
    if (!repository.create_group(stored, error)) {
        set_note("Não foi possível criar o separador: " + error, NoteKind::error);
        return;
    }
    auto group = loaded_group(stored);
    std::scoped_lock lock(sessions_mutex);
    auto items = container_items_unlocked(0);
    groups.insert(groups.begin(), group);
    items.insert(items.begin(), QueueKey{true, group->id});
    persist_order_locked(items, 0);
}

void State::save_group(const std::shared_ptr<Group>& group) {
    if (!group) return;
    std::string error;
    if (!repository.save_group(stored_group(*group), error)) {
        set_note("Não foi possível salvar o separador: " + error, NoteKind::error);
    }
}

void State::delete_group(long long group_id) {
    std::string error;
    std::scoped_lock lock(sessions_mutex);
    const auto group_it = std::find_if(groups.begin(), groups.end(), [group_id](const auto& group) {
        return group->id == group_id;
    });
    if (group_it == groups.end()) return;

    auto children = container_items_unlocked(group_id);
    auto top_items = container_items_unlocked(0);
    top_items.erase(std::remove(top_items.begin(), top_items.end(), QueueKey{true, group_id}), top_items.end());
    for (const auto& child : children) top_items.push_back(child);

    groups.erase(group_it);
    if (!repository.delete_group(group_id, error)) {
        set_note("Não foi possível remover o separador: " + error, NoteKind::error);
        return;
    }
    persist_order_locked(top_items, 0);
}

std::shared_ptr<Session> State::next_queued_session() {
    std::scoped_lock lock(sessions_mutex);
    for (const auto& key : container_items_unlocked(0)) {
        if (key.is_group) {
            for (const auto& child : container_items_unlocked(key.id)) {
                auto session = find_session_unlocked(child.id);
                if (session && session->status == Status::queued) return session;
            }
        } else {
            auto session = find_session_unlocked(key.id);
            if (session && session->status == Status::queued) return session;
        }
    }
    return nullptr;
}

void State::remove_selected_session() {
    if (!selected || selected->busy()) return;
    const auto removed = selected;
    std::string error;
    if (!repository.delete_session(removed->id, error)) {
        set_note("Não foi possível remover a transcrição: " + error, NoteKind::error);
        return;
    }
    {
        std::scoped_lock lock(sessions_mutex);
        sessions.erase(std::remove(sessions.begin(), sessions.end(), removed), sessions.end());
        set_single_selection(sessions.empty() ? nullptr : sessions.front());
    }
    if (!selected) create_session();
    set_note("Conteúdo removido com sucesso.");
}

void State::toggle_selected_queue() {
    if (!selected || selected->busy() || selected->input.empty()) return;
    selected->status = selected->status == Status::queued ? Status::paused : Status::queued;
    selected->set_message(selected->status == Status::queued
        ? "Transcrição adicionada à fila."
        : "Transcrição pausada. Clique em Transcrever para reativar.");
    save_session(selected);
    start_queue_worker();
}

void State::set_single_selection(const std::shared_ptr<Session>& session) {
    selected = session;
    queue_selection.clear();
    if (session) {
        queue_selection.push_back(session->id);
        selection_anchor_id = session->id;
    } else {
        selection_anchor_id = 0;
    }
}

bool State::is_session_selected(long long id) const {
    return std::find(queue_selection.begin(), queue_selection.end(), id) != queue_selection.end();
}

// Implements Ctrl/Shift-click multi-selection: a plain click selects just this
// session, Ctrl toggles it in/out of the selection, and Shift selects the
// contiguous range (in queue order) between the last anchor and this session.
// The detail panel always follows the last-clicked session.
void State::handle_session_click(long long session_id, bool ctrl, bool shift) {
    std::scoped_lock lock(sessions_mutex);
    auto session = find_session_unlocked(session_id);
    if (!session) return;
    selected = session;

    if (shift && selection_anchor_id != 0) {
        const auto order = global_session_order_unlocked();
        const auto anchor_it = std::find(order.begin(), order.end(), selection_anchor_id);
        const auto target_it = std::find(order.begin(), order.end(), session_id);
        if (anchor_it != order.end() && target_it != order.end()) {
            const auto [first_it, last_it] = std::minmax(anchor_it, target_it);
            queue_selection.assign(first_it, last_it + 1);
            return;
        }
    }

    if (ctrl) {
        const auto it = std::find(queue_selection.begin(), queue_selection.end(), session_id);
        if (it != queue_selection.end()) {
            queue_selection.erase(it);
        } else {
            queue_selection.push_back(session_id);
        }
        selection_anchor_id = session_id;
        return;
    }

    queue_selection = {session_id};
    selection_anchor_id = session_id;
}

// Builds the drag-drop payload for a session card: when the dragged card is
// part of a multi-selection, the whole selection is carried as a block (in
// queue order) so it moves together; otherwise just this one session.
std::vector<QueueKey> State::drag_payload_for(long long session_id) {
    std::scoped_lock lock(sessions_mutex);
    if (!is_session_selected(session_id) || queue_selection.size() <= 1) {
        return {QueueKey{false, session_id}};
    }
    std::vector<QueueKey> keys;
    for (long long id : global_session_order_unlocked()) {
        if (is_session_selected(id)) keys.push_back(QueueKey{false, id});
    }
    return keys;
}

// Sets the queued/paused status of every selected session that is eligible
// (not currently running, has an input file) — used by the batch
// "adicionar à fila"/"pausar" context-menu actions.
void State::set_selection_status(Status new_status) {
    std::string error;
    bool changed = false;
    {
        std::scoped_lock lock(sessions_mutex);
        const bool in_transaction = repository.begin_transaction(error);
        for (long long id : queue_selection) {
            auto session = find_session_unlocked(id);
            if (!session || session->busy() || session->input.empty()) continue;
            session->status = new_status;
            session->set_message(new_status == Status::queued
                ? "Transcrição adicionada à fila."
                : "Transcrição pausada. Clique em Transcrever para reativar.");
            save_session(session);
            changed = true;
        }
        if (in_transaction && !repository.commit_transaction(error)) {
            set_note("Não foi possível salvar a fila: " + error, NoteKind::error);
        }
    }
    if (changed) start_queue_worker();
}

// Decides whether "Pausar" can apply directly or needs confirmation first:
// pausing a session that is currently transcribing requires cancelling the
// in-progress run, so the user is asked before that happens.
void State::request_pause_selection() {
    bool has_running = false;
    {
        std::scoped_lock lock(sessions_mutex);
        for (long long id : queue_selection) {
            auto session = find_session_unlocked(id);
            if (session && session->busy()) {
                has_running = true;
                break;
            }
        }
    }
    if (has_running) {
        pause_running_confirmation = true;
        return;
    }
    set_selection_status(Status::paused);
}

// Decides whether "Adicionar à fila" can apply directly or needs confirmation
// first: re-queuing a session that already has a transcript will overwrite
// that text once it is reprocessed, so the user is asked before that happens.
void State::request_queue_selection() {
    bool overwrites_transcript = false;
    {
        std::scoped_lock lock(sessions_mutex);
        for (long long id : queue_selection) {
            auto session = find_session_unlocked(id);
            if (session && !session->busy() && !session->input.empty() && !session->transcript.empty()) {
                overwrites_transcript = true;
                break;
            }
        }
    }
    if (overwrites_transcript) {
        requeue_overwrite_confirmation = true;
        return;
    }
    set_selection_status(Status::queued);
}

// Confirms the "Pausar" action on a selection that includes the running
// session: cancels the in-progress transcription (which lands on
// Status::paused instead of Status::cancelled, see process_session) and
// pauses the rest of the selection immediately.
void State::confirm_pause_running() {
    pause_after_cancel = true;
    cancel_requested = true;
    set_selection_status(Status::paused);
    pause_running_confirmation = false;
}

// Removes every selected, non-busy session in a single transaction, then
// falls back to a fresh draft session if the queue ends up empty.
void State::remove_queue_selection() {
    std::string error;
    bool need_new_session = false;
    {
        std::scoped_lock lock(sessions_mutex);
        const bool in_transaction = repository.begin_transaction(error);
        for (long long id : queue_selection) {
            auto session = find_session_unlocked(id);
            if (!session || session->busy()) continue;
            if (!repository.delete_session(id, error)) {
                set_note("Não foi possível remover a transcrição: " + error, NoteKind::error);
                continue;
            }
            sessions.erase(std::remove(sessions.begin(), sessions.end(), session), sessions.end());
        }
        if (in_transaction && !repository.commit_transaction(error)) {
            set_note("Não foi possível remover as transcrições: " + error, NoteKind::error);
        }
        const bool selection_lost = !selected || std::find(sessions.begin(), sessions.end(), selected) == sessions.end();
        set_single_selection(selection_lost ? (sessions.empty() ? nullptr : sessions.front()) : selected);
        need_new_session = sessions.empty();
    }
    if (need_new_session) create_session();
    set_note("Conteúdo removido com sucesso.");
}

void State::start_queue_worker() {
    {
        std::scoped_lock lock(sessions_mutex);
        if (std::none_of(sessions.begin(), sessions.end(), [](const auto& session) {
            return session->status == Status::queued;
        })) return;
    }
    if (worker_running.exchange(true)) return;
    if (worker.joinable()) worker.join();
    worker = std::jthread([this] { worker_loop(); });
}

void State::worker_loop() {
    try {
        while (true) {
            std::shared_ptr<Session> next = next_queued_session();
            if (!next) break;
            process_session(next);
        }
    } catch (...) {}
    worker_running = false;
}

void State::process_session(const std::shared_ptr<Session>& session) {
    if (!session || session->input.empty()) return;
    const auto selected_input = session->input;
    cancel_requested = false;
    session->transcribing = false;
    session->progress = 0.0F;
    session->whisper_progress = 0.0F;
    session->projected_total_seconds = 0.0;
    session->status = Status::running;
    session->transcript.clear();
    session->transcript_annotated.clear();
    session->transcript_display.clear();
    session->transcript_wrap_width = 0.0F;
    session->transcript_needs_wrap = false;
    session->set_message("Preparando arquivo de áudio...");
    save_session(session);

    // Use the local TEMP directory instead of the exe directory so that temp
    // files are always on a local drive — whisper-cli.exe and its MinGW DLLs
    // are also launched from their own directory (passed as working_directory to
    // CreateProcess) which avoids Windows DLL-loading restrictions on network shares.
    const auto temporary_directory = std::filesystem::temp_directory_path() / L"sto_transcription";
    {
        std::error_code ec;
        std::filesystem::create_directories(temporary_directory, ec);
        if (ec) {
            session->transcribing = false;
            session->status = Status::failed;
            session->set_message("Não foi possível criar o diretório temporário: " + temporary_directory.string());
            save_session(session);
            return;
        }
    }
    const auto wav = sto::files::unique_path(temporary_directory / (sto::files::sanitize_stem(selected_input.stem().wstring()) + L".wav"));
    const auto txt = std::filesystem::path(wav.wstring() + L".txt");
    const double media_duration = probe_duration(selected_input);

    const auto fail = [this, &session, &wav, &txt](Status status, std::string message) {
        session->transcribing = false;
        session->status = status;
        session->set_message(std::move(message));
        save_session(session);
        cleanup(wav, txt);
    };

    // A cancellation triggered via the queue's "Pausar" action lands on
    // Status::paused (so "Transcrever" resumes it) instead of the terminal
    // Status::cancelled used for a plain "Cancelar transcrição".
    const auto fail_cancelled = [this, &fail]() {
        if (pause_after_cancel.exchange(false)) {
            fail(Status::paused, "Transcrição pausada. Clique em Transcrever para reativar.");
        } else {
            fail(Status::cancelled, "Transcrição cancelada pelo usuário.");
        }
    };

    const auto conversion = sto::platform::run_process({
        runtime(L"runtime/ffmpeg/ffmpeg.exe").wstring(), L"-hide_banner", L"-nostats", L"-y", L"-i", selected_input.wstring(),
        L"-vn", L"-ar", L"16000", L"-ac", L"1", L"-c:a", L"pcm_s16le", L"-progress", L"pipe:1", wav.wstring()
    }, cancel_requested);
    if (conversion.cancelled) {
        fail_cancelled();
        return;
    }
    if (conversion.exit_code != 0 || !std::filesystem::exists(wav)) {
        fail(Status::failed, "Não foi possível preparar o áudio com o FFmpeg.");
        return;
    }

    std::string estimate_error;
    session->estimated_seconds = repository.estimate_seconds(processor_model, media_duration, estimate_error);
    session->projected_total_seconds = session->estimated_seconds.load();
    session->transcribing = true;
    session->progress = 0.0F;
    session->whisper_progress = 0.0F;
    session->transcription_started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    session->set_message({});
    const auto transcription_started = std::chrono::steady_clock::now();
    const auto whisper_dir = runtime(L"runtime/whisper");
    const auto whisper = sto::platform::run_process({
        (whisper_dir / L"whisper-cli.exe").wstring(),
        L"-m", (whisper_dir / L"ggml-large-v3-turbo.bin").wstring(),
        L"-f", wav.wstring(), L"-l", L"pt", L"-otxt", L"-np", L"-pp",
        L"-t", std::to_wstring(std::max(1U, std::thread::hardware_concurrency())), L"-p", L"1"
    }, cancel_requested, [session](const std::string& line) {
        const auto percent = line.find('%');
        if (percent == std::string::npos) return;
        const auto start = line.find_last_not_of("0123456789", percent == 0 ? 0 : percent - 1);
        try {
            const float value = std::stof(line.substr(start == std::string::npos ? 0 : start + 1));
            session->whisper_progress = std::clamp(value / 100.0F, 0.0F, 1.0F);
            session->progress = session->whisper_progress.load();
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const double elapsed = static_cast<double>(now - session->transcription_started_ms.load()) / 1000.0;
            const double reported = session->whisper_progress.load();
            if (reported >= 0.01 && reported < 1.0 && elapsed > 0.0) {
                const double implied_total = elapsed / reported;
                const double historical_total = session->estimated_seconds.load();
                const double anchor_weight = std::clamp(reported, 0.20, 0.85);
                session->projected_total_seconds = (historical_total * (1.0 - anchor_weight)) + (implied_total * anchor_weight);
            }
        } catch (...) {}
    }, whisper_dir.wstring());
    session->transcribing = false;
    if (whisper.cancelled) {
        fail_cancelled();
        return;
    }
    if (!whisper.started) {
        fail(Status::failed,
            "O Whisper não pôde ser iniciado. Verifique se whisper-cli.exe e suas "
            "DLLs estão presentes em runtime\\whisper\\.");
        return;
    }
    if (whisper.exit_code != 0 || !std::filesystem::exists(txt)) {
        // Build a diagnostic message: include exit code and the last chunk of
        // whisper's output so the user (or support) can see what actually failed.
        std::string diag = "O Whisper falhou (código " + std::to_string(whisper.exit_code) + ").";
        if (!whisper.output.empty()) {
            const std::size_t tail = whisper.output.size() > 300
                ? whisper.output.size() - 300 : 0;
            std::string snippet = whisper.output.substr(tail);
            // trim leading whitespace/newlines from snippet
            const auto first = snippet.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) snippet = snippet.substr(first);
            if (!snippet.empty()) diag += " Saída: " + snippet;
        }
        fail(Status::failed, diag);
        return;
    }

    const auto echo_result = process_echo_repetitions(read_text(txt));
    session->transcript = echo_result.clean;
    session->transcript_annotated = echo_result.annotated;
    session->transcript_needs_wrap = true;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - transcription_started).count();
    if (media_duration > 0.0 && elapsed > 0.0) {
        std::string history_error;
        repository.record_history(processor_model, media_duration, elapsed, history_error);
    }
    cleanup(wav, txt);
    session->progress = 1.0F;
    session->status = Status::completed;
    session->set_message("Transcrição concluída. Revise o texto antes de copiar.");
    save_session(session);
    set_note("Transcrição concluída com sucesso.");
    show_windows_notification(
        L"Transcrição concluída",
        L"A oitiva foi transcrita com sucesso.\nO texto já está disponível para conferência."
    );
}

bool has_meaningful_text(const char* text) {
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return true;
    }
    return false;
}

std::string composed_prompt(const State& module, const Session& session) {
    std::string output;
    if (has_meaningful_text(session.additional_context.c_str())) {
        output += module.prompts.context;
        output += "\n\nContextualização:\n\"";
        output += session.additional_context;
        output += "\"\n\n";
    }
    output += session.procedure == 0 ? module.prompts.traditional : module.prompts.formal;
    output += "\n\nTipo de oitiva: ";
    output += kHearingTypes[static_cast<std::size_t>(session.hearing_type)];
    output += "\nNome da parte: ";
    output += session.party_name[0] == '\0' ? "Não informado" : session.party_name.data();
    output += "\n\nTexto bruto extraído do Whisper:\n\"";
    output += session.transcript;
    output += "\"";
    return output;
}

float visual_progress(const Session& session) {
    const float reported = session.whisper_progress.load();
    if (!session.transcribing || reported >= 1.0F) return session.progress.load();

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const double elapsed = static_cast<double>(now - session.transcription_started_ms.load()) / 1000.0;
    if (elapsed <= 0.0) return reported;

    const double projected_total = session.projected_total_seconds.load();
    if (projected_total <= 0.0) return reported;
    const float projected = static_cast<float>(elapsed / projected_total);
    return std::clamp(std::max(projected, reported), reported, 0.99F);
}

double remaining_seconds(const Session& session) {
    if (!session.transcribing) return session.estimated_seconds.load();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double elapsed = static_cast<double>(now - session.transcription_started_ms.load()) / 1000.0;
    return std::max(0.0, session.projected_total_seconds.load() - elapsed);
}

std::string remaining_text(const Session& session) {
    const double seconds = remaining_seconds(session);
    if (session.transcribing && seconds < 1.0 && session.progress.load() < 1.0F) return "quase pronto";
    return format_duration(seconds);
}

void render_progress(Session& session) {
    if (!session.busy()) return;
    ImGui::Spacing();
    ImGui::ProgressBar(visual_progress(session), {-1.0F, 22.0F});
}

// Like reflow_paragraph but strips \x01 echo markers when measuring text
// width while preserving them in the output, so line breaks fall at the same
// positions as they would in the clean (marker-free) text.
static std::string reflow_annotated(const std::string& paragraph, float width) {
    std::vector<std::string> words;
    {
        std::istringstream ss(paragraph);
        std::string w;
        while (ss >> w) words.push_back(w);
    }
    std::string result;
    std::string cur;
    std::string cur_vis; // markers stripped, used only for CalcTextSize
    for (const auto& word : words) {
        std::string vis;
        for (unsigned char c : word) if (c != 0x01) vis += static_cast<char>(c);
        const std::string cand     = cur.empty()     ? word : cur     + " " + word;
        const std::string cand_vis = cur_vis.empty() ? vis  : cur_vis + " " + vis;
        if (!cur.empty() && ImGui::CalcTextSize(cand_vis.c_str()).x > width) {
            if (!result.empty()) result += '\n';
            result += cur;
            cur     = word;
            cur_vis = vis;
        } else {
            cur     = cand;
            cur_vis = cand_vis;
        }
    }
    if (!cur.empty()) {
        if (!result.empty()) result += '\n';
        result += cur;
    }
    return result;
}

// Like wrap_text but handles \x01 echo markers using reflow_annotated so the
// layout is identical to the plain-text version.
static std::string wrap_annotated(const std::string& text, float width) {
    std::istringstream input(text);
    std::string output;
    std::string paragraph;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            if (!paragraph.empty()) {
                if (!output.empty()) output += '\n';
                output += reflow_annotated(paragraph, width);
                paragraph.clear();
            }
            if (!output.empty() && !output.ends_with("\n\n")) output += '\n';
        } else {
            if (!paragraph.empty()) paragraph += ' ';
            paragraph += line;
        }
    }
    if (!paragraph.empty()) {
        if (!output.empty() && !output.ends_with('\n')) output += '\n';
        output += reflow_annotated(paragraph, width);
    }
    return output;
}

static void render_calendar_popup_oitiva(State& module, Session& session) {
    namespace chrono = std::chrono;
    if (!ImGui::BeginPopup("calendar-oitiva-popup")) return;

    const int year = static_cast<int>(module.calendar_view_oitiva.year());
    const unsigned month = static_cast<unsigned>(module.calendar_view_oitiva.month());

    if (ImGui::Button("<", {28.0F, 0.0F})) module.calendar_view_oitiva -= chrono::months{1};
    ImGui::SameLine();
    std::string mname = sto::text::month_name_pt(month);
    if (!mname.empty()) mname[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(mname[0])));
    const std::string month_label = mname + " " + std::to_string(year);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(month_label.c_str()).x) * 0.5F);
    ImGui::TextColored(kWhite, "%s", month_label.c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 28.0F - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button(">", {28.0F, 0.0F})) module.calendar_view_oitiva += chrono::months{1};
    ImGui::Spacing();

    if (ImGui::BeginTable("calendar-oitiva-grid", 7, ImGuiTableFlags_SizingFixedFit)) {
        for (const char* wd : {"D", "S", "T", "Q", "Q", "S", "S"}) {
            ImGui::TableNextColumn();
            ImGui::TextColored(kMuted, "%s", wd);
        }
        const chrono::year_month_day first_day{module.calendar_view_oitiva / chrono::day{1}};
        const chrono::weekday first_weekday{chrono::sys_days{first_day}};
        const chrono::year_month_day_last last_day{module.calendar_view_oitiva / chrono::last};
        const unsigned days = static_cast<unsigned>(last_day.day());
        for (unsigned lead = 0; lead < first_weekday.c_encoding(); ++lead) ImGui::TableNextColumn();
        for (unsigned day = 1; day <= days; ++day) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(day));
            char dlabel[4];
            std::snprintf(dlabel, sizeof(dlabel), "%u", day);
            if (ImGui::Button(dlabel, {30.0F, 26.0F})) {
                std::snprintf(session.data_oitiva.data(), session.data_oitiva.size(), "%02u/%02u/%04d", day, month, year);
                module.save_session(module.selected);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndPopup();
}

// Renders pre-wrapped annotated transcript text line by line.
// Echo segments (between \x01 toggle markers) are coloured red.
// The text must be pre-wrapped (via wrap_annotated) so each visual line fits
// on screen — this lets us use SameLine only within a single line, avoiding
// the multi-line SameLine positioning bug.
static void render_annotated_transcript(const char* annotated) {
    bool is_echo = false;
    const char* p = annotated;

    while (true) {
        // Find end of this visual line.
        const char* line_end = p;
        while (*line_end && *line_end != '\n') ++line_end;

        // Render colour segments within [p, line_end).
        bool first_on_line = true;
        const char* seg_start = p;

        for (const char* q = p; q <= line_end; ++q) {
            const bool at_end    = (q == line_end);
            const bool at_marker = (!at_end && *q == '\x01');
            if (at_end || at_marker) {
                if (q > seg_start) {
                    if (!first_on_line) ImGui::SameLine(0.0F, 0.0F);
                    if (is_echo) ImGui::PushStyleColor(ImGuiCol_Text, kError);
                    ImGui::TextUnformatted(seg_start, q);
                    if (is_echo) ImGui::PopStyleColor();
                    first_on_line = false;
                }
                if (at_marker) {
                    is_echo = !is_echo;
                    seg_start = q + 1;
                }
            }
        }
        if (first_on_line) ImGui::TextUnformatted(p, line_end); // blank line

        if (!*line_end) break;
        p = line_end + 1; // skip '\n'
    }
}

void render_station(State& module, Session& session) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const bool compact = available.y < 720.0F;
    const bool narrow_layout = available.x < 930.0F;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, compact ? ImVec2{14.0F, 12.0F} : ImVec2{18.0F, 18.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, compact ? ImVec2{10.0F, 6.0F} : ImVec2{12.0F, 10.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, compact ? ImVec2{10.0F, 6.0F} : ImVec2{12.0F, 8.0F});
    const ImGuiStyle& style = ImGui::GetStyle();
    constexpr ImGuiWindowFlags fixed_panel_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const float file_panel_height =
        (style.WindowPadding.y * 2.0F) + ImGui::GetTextLineHeight() + style.ItemSpacing.y + ImGui::GetFrameHeight();
    ImGui::BeginChild("file-selector", {0.0F, file_panel_height}, ImGuiChildFlags_Borders, fixed_panel_flags);
    ImGui::TextColored(kSilver, "%s  ARQUIVO DE ÁUDIO OU VÍDEO", kIconFolder);
    ImGui::Spacing();
    constexpr float select_width = 154.0F;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - select_width - ImGui::GetStyle().ItemSpacing.x);
    std::array<char, 1024> selected_file{};
    std::snprintf(selected_file.data(), selected_file.size(), "%s", session.input.empty() ? "Nenhum arquivo selecionado..." : narrow(session.input.wstring()).c_str());
    ImGui::InputText("##selected-file", selected_file.data(), selected_file.size(), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    ImGui::BeginDisabled(session.busy() || session.status == Status::queued);
    if (ImGui::Button(label(kIconFolder, "Selecionar").c_str(), {select_width, 0.0F})) {
        const auto files = sto::platform::select_files(L"Selecione o arquivo da oitiva", {
            {L"Arquivos de áudio e vídeo", L"*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.opus;*.wma;*.mp4;*.avi;*.mkv;*.mov;*.webm"},
            {L"Todos os arquivos", L"*.*"}
        });
        if (!files.empty()) {
            if (files.size() == 1) {
                session.input = files.front();
                session.status = Status::idle;
                session.transcript.clear();
                session.transcript_annotated.clear();
                session.transcript_display.clear();
                session.transcript_wrap_width = 0.0F;
                session.transcript_needs_wrap = false;
                session.data_oitiva.fill('\0');
                {
                    const auto det = detect_from_filename(files.front());
                    if (det.hearing_type >= 0) session.hearing_type = det.hearing_type;
                    if (!det.party_name.empty())
                        std::snprintf(session.party_name.data(), session.party_name.size(), "%s", det.party_name.c_str());
                    if (!det.date.empty())
                        std::snprintf(session.data_oitiva.data(), session.data_oitiva.size(), "%s", det.date.c_str());
                }
                session.set_message("Arquivo selecionado. Preencha os dados e inicie a transcrição.");
                module.save_session(module.selected);
            } else {
                std::shared_ptr<Session> first_added;
                bool used_current = false;
                if (session.status == Status::idle && session.input.empty() && session.transcript.empty()) {
                    session.input = files.front();
                    {
                        const auto det = detect_from_filename(files.front());
                        if (det.hearing_type >= 0) session.hearing_type = det.hearing_type;
                        if (!det.party_name.empty())
                            std::snprintf(session.party_name.data(), session.party_name.size(), "%s", det.party_name.c_str());
                        if (!det.date.empty())
                            std::snprintf(session.data_oitiva.data(), session.data_oitiva.size(), "%s", det.date.c_str());
                    }
                    session.status = Status::paused;
                    session.set_message("Transcrição pausada. Revise os dados e clique em Transcrever para iniciar.");
                    session.transcript_needs_wrap = false;
                    module.save_session(module.selected);
                    first_added = module.selected;
                    used_current = true;
                }
                for (std::size_t index = used_current ? 1U : 0U; index < files.size(); ++index) {
                    auto created = module.create_session_for_file(files[index], Status::paused);
                    if (created) {
                        const auto det = detect_from_filename(files[index]);
                        if (det.hearing_type >= 0) created->hearing_type = det.hearing_type;
                        if (!det.party_name.empty())
                            std::snprintf(created->party_name.data(), created->party_name.size(), "%s", det.party_name.c_str());
                        if (!det.date.empty())
                            std::snprintf(created->data_oitiva.data(), created->data_oitiva.size(), "%s", det.date.c_str());
                        module.save_session(created);
                    }
                    if (!first_added) first_added = created;
                }
                if (first_added) module.set_single_selection(first_added);
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::BeginDisabled(session.busy() || session.status == Status::queued);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.095F, 0.105F, 0.115F, 1.00F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.16F, 0.17F, 0.18F, 1.00F});
    if (ImGui::Button(label(session.show_context ? kIconChevronDown : kIconChevronRight, "Contexto adicional (opcional)").c_str(), {-1.0F, 34.0F})) {
        session.show_context = !session.show_context;
        module.save_session(module.selected);
    }
    ImGui::PopStyleColor(2);
    if (session.show_context) {
        ImGui::InputTextMultiline("##additional-context", &session.additional_context, {-1.0F, compact ? 72.0F : 112.0F});
        if (ImGui::IsItemDeactivatedAfterEdit()) module.save_session(module.selected);
    }

    ImGui::Spacing();
    constexpr float kCalBtnW = 38.0F;
    const float information_panel_height =
        (style.WindowPadding.y * 2.0F)
        + ImGui::GetTextLineHeight() + style.ItemSpacing.y   // "INFORMAÇÕES ADICIONAIS"
        + ImGui::GetFrameHeight() + style.ItemSpacing.y      // Nome da parte (label-left)
        + ImGui::GetTextLineHeight() + style.ItemSpacing.y + ImGui::GetFrameHeight() + style.ItemSpacing.y  // Tipo+Data (label-above, 2 cols)
        + ImGui::GetFrameHeight();                           // Tipo de procedimento (label-left)
    ImGui::BeginChild("hearing-data", {0.0F, information_panel_height}, ImGuiChildFlags_Borders, fixed_panel_flags);
    ImGui::TextColored(kSilver, "%s  INFORMAÇÕES ADICIONAIS", kIconMicrophone);
    ImGui::Spacing();
    constexpr float label_width = 210.0F;
    ImGui::TextColored(kMuted, "Nome da parte ouvida");
    ImGui::SameLine(label_width);
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputText("##party-name", session.party_name.data(), session.party_name.size());
    if (ImGui::IsItemDeactivatedAfterEdit()) module.save_session(module.selected);

    // Tipo de oitiva (left) | Data da oitiva (right) — same row, label above each
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float left_w = (avail - style.ItemSpacing.x) * 0.45F;
        const float right_w = avail - left_w - style.ItemSpacing.x;
        const ImVec2 row = ImGui::GetCursorPos();

        ImGui::SetCursorPos(row);
        ImGui::TextColored(kMuted, "Tipo de oitiva");
        ImGui::SetCursorPos({row.x, ImGui::GetCursorPosY()});
        ImGui::SetNextItemWidth(left_w);
        if (ImGui::Combo("##hearing-type", &session.hearing_type, kHearingTypes.data(), static_cast<int>(kHearingTypes.size())))
            module.save_session(module.selected);
        const float left_bottom = ImGui::GetCursorPosY();

        const ImVec2 rpos = {row.x + left_w + style.ItemSpacing.x, row.y};
        ImGui::SetCursorPos(rpos);
        ImGui::TextColored(kMuted, "Data da oitiva");
        ImGui::SetCursorPos({rpos.x, ImGui::GetCursorPosY()});
        ImGui::SetNextItemWidth(right_w - kCalBtnW - style.ItemSpacing.x);
        if (ImGui::InputTextWithHint("##oitiva-date", "DD/MM/AAAA", session.data_oitiva.data(), session.data_oitiva.size(),
                ImGuiInputTextFlags_CallbackEdit, sto::text::date_mask_callback))
            module.save_session(module.selected);
        ImGui::SameLine();
        if (ImGui::Button(kIconCalendar, {kCalBtnW, 0.0F})) {
            if (const auto d = sto::text::parse_date_ddmmyyyy(session.data_oitiva.data()))
                module.calendar_view_oitiva = d->year() / d->month();
            ImGui::OpenPopup("calendar-oitiva-popup");
        }
        render_calendar_popup_oitiva(module, session);
        ImGui::SetCursorPos({row.x, std::max(left_bottom, ImGui::GetCursorPosY())});
    }

    ImGui::TextColored(kMuted, "Tipo de procedimento");
    ImGui::SameLine(label_width);
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::Combo("##procedure", &session.procedure, kProcedures.data(), static_cast<int>(kProcedures.size()))) module.save_session(module.selected);
    ImGui::EndChild();
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextColored(kSilver, "TEXTO DA TRANSCRIÇÃO");
    ImGui::Spacing();
    const float action_button_height = compact ? 36.0F : 42.0F;
    const float bottom_accommodation = compact ? 10.0F : 18.0F;
    const float action_area_height = action_button_height + (style.ItemSpacing.y * 3.0F) + 1.0F + bottom_accommodation;
    const float progress_area_height = session.busy()
        ? style.ItemSpacing.y + ImGui::GetTextLineHeight() + style.ItemSpacing.y + 22.0F
        : 0.0F;
    const float transcript_height = std::max(42.0F, ImGui::GetContentRegionAvail().y - action_area_height - progress_area_height);
    ImGui::BeginChild("transcription-result", {0.0F, transcript_height}, ImGuiChildFlags_Borders);
    if (session.busy()) {
        const int dots = static_cast<int>(ImGui::GetTime() * 2.0) % 5 + 1;
        ImGui::SetCursorPosY(std::max(12.0F, transcript_height * 0.17F));
        ImGui::SetWindowFontScale(1.18F);
        ImGui::TextColored(kBlue, "%s", kIconGear);
        ImGui::SameLine();
        if (sto::ui::section_font()) ImGui::PushFont(sto::ui::section_font());
        if (session.transcribing) {
            ImGui::TextColored(kBlue, "Transcrevendo %s", kProcedures[static_cast<std::size_t>(session.procedure)]);
            if (session.party_name[0] != '\0') ImGui::TextColored(kGreen, "Parte ouvida: %s", session.party_name.data());
            ImGui::TextColored(kSilver, "Tempo restante estimado: %s", remaining_text(session).c_str());
        } else {
            ImGui::TextColored(kBlue, "Preparando arquivo de áudio");
        }
        ImGui::TextColored(kMuted, "Aguarde%s", std::string(static_cast<std::size_t>(dots), '.').c_str());
        if (sto::ui::section_font()) ImGui::PopFont();
        ImGui::SetWindowFontScale(1.00F);
    } else {
        const float wrap_width = std::max(120.0F, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2.0F - sto::ui::kWrapRightMargin);
        if (session.transcript_needs_wrap.exchange(false) || std::abs(session.transcript_wrap_width - wrap_width) > 1.0F) {
            session.transcript_display = wrap_annotated(session.transcript_annotated.c_str(), wrap_width);
            session.transcript_wrap_width = wrap_width;
        }
        render_annotated_transcript(session.transcript_display.c_str());
    }
    ImGui::EndChild();

    render_progress(session);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float button_spacing = ImGui::GetStyle().ItemSpacing.x;
    const float button_width = (ImGui::GetContentRegionAvail().x - (button_spacing * 2.0F)) / 3.0F;
    const char* transcribe_text = session.busy()
        ? (narrow_layout ? "Cancelar" : "Cancelar transcrição")
        : session.status == Status::queued ? (narrow_layout ? "Pausar" : "Pausar na fila") : "Transcrever";
    const char* copy_text = narrow_layout ? "Copiar" : "Copiar prompt final";
    const char* clear_text = narrow_layout ? "Limpar" : "Limpar conteúdo";
    ImGui::BeginDisabled(!session.busy() && session.input.empty());
    if (colored_button(
        label(session.busy() ? kIconCancel : session.status == Status::queued ? kIconPause : kIconPlay, transcribe_text),
        {button_width, action_button_height},
        session.busy() ? kYellow : kBlue,
        session.busy() ? kDarkText : kWhite
    )) {
        if (session.busy()) module.cancel_confirmation = true;
        else if (session.status == Status::queued) module.toggle_selected_queue();
        else if (!session.transcript.empty()) module.retranscribe_confirmation = true;
        else module.toggle_selected_queue();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(session.busy() || session.transcript.empty());
    if (colored_button(label(kIconCopy, copy_text), {button_width, action_button_height}, kGreen)) {
        const bool copied = copy_to_clipboard(composed_prompt(module, session));
        module.set_note(
            copied ? "Prompt final copiado para a área de transferência." : "Não foi possível acessar a área de transferência.",
            copied ? NoteKind::success : NoteKind::error
        );
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(session.busy() || session.status == Status::queued);
    if (colored_button(label(kIconTrash, clear_text), {button_width, action_button_height}, kRed)) module.clear_confirmation = true;
    ImGui::EndDisabled();

    if (module.cancel_confirmation) ImGui::OpenPopup("Confirmar cancelamento");
    if (ImGui::BeginPopupModal("Confirmar cancelamento", &module.cancel_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Deseja realmente cancelar a transcrição em andamento?");
        ImGui::Spacing();
        if (ImGui::Button("Continuar transcrevendo", {190.0F, 36.0F})) module.cancel_confirmation = false;
        ImGui::SameLine();
        if (colored_button("Cancelar transcrição", {170.0F, 36.0F}, kYellow, kDarkText)) {
            module.cancel_requested = true;
            module.cancel_confirmation = false;
        }
        ImGui::EndPopup();
    }
    if (module.retranscribe_confirmation) ImGui::OpenPopup("Confirmar nova transcrição");
    if (ImGui::BeginPopupModal("Confirmar nova transcrição", &module.retranscribe_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Já existe um texto transcrito. Deseja iniciar uma nova transcrição?");
        ImGui::TextColored(kSilver, "O conteúdo atual será substituído.");
        ImGui::Spacing();
        const float popup_button_spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttons_width = 150.0F + popup_button_spacing + 180.0F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (ImGui::GetContentRegionAvail().x - buttons_width) * 0.5F));
        if (ImGui::Button("Manter conteúdo", {150.0F, 36.0F})) module.retranscribe_confirmation = false;
        ImGui::SameLine();
        if (colored_button("Transcrever novamente", {180.0F, 36.0F}, kBlue)) {
            module.retranscribe_confirmation = false;
            module.toggle_selected_queue();
        }
        ImGui::EndPopup();
    }
    if (module.clear_confirmation) ImGui::OpenPopup("Confirmar limpeza");
    if (ImGui::BeginPopupModal("Confirmar limpeza", &module.clear_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Deseja realmente limpar todo o conteúdo da transcrição?");
        ImGui::Spacing();
        const float popup_button_spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttons_width = 150.0F + popup_button_spacing + 150.0F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (ImGui::GetContentRegionAvail().x - buttons_width) * 0.5F));
        if (ImGui::Button("Manter conteúdo", {150.0F, 36.0F})) module.clear_confirmation = false;
        ImGui::SameLine();
        if (colored_button("Limpar conteúdo", {150.0F, 36.0F}, kRed)) {
            module.remove_selected_session();
            module.clear_confirmation = false;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
}

ImVec4 session_color(Status status) {
    switch (status) {
    case Status::running: return kBlue;
    case Status::queued: return kYellow;
    case Status::completed: return kGreen;
    case Status::failed: return kRed;
    case Status::cancelled: return {0.78F, 0.36F, 0.20F, 1.00F};
    case Status::paused: return kPurple;
    default: return kSilver;
    }
}

const char* session_icon(Status status) {
    switch (status) {
    case Status::running: return kIconGear;
    case Status::queued: return kIconClock;
    case Status::completed: return kIconCheck;
    case Status::failed:
    case Status::cancelled: return kIconCancel;
    case Status::paused: return kIconPause;
    default: return kIconMedia;
    }
}

const char* session_status(Status status) {
    switch (status) {
    case Status::running: return "EM ANDAMENTO";
    case Status::queued: return "NA FILA";
    case Status::completed: return "CONCLUÍDA";
    case Status::failed: return "FALHOU";
    case Status::cancelled: return "CANCELADA";
    case Status::paused: return "PAUSADA";
    default: return "RASCUNHO";
    }
}

std::string session_status_text(const Session& session) {
    const Status status = session.status.load();
    if (status != Status::running) return session_status(status);
    const int percent = static_cast<int>(std::round(visual_progress(session) * 100.0F));
    return std::format("EM ANDAMENTO ({}%)", std::clamp(percent, 0, 100));
}

std::string session_title(const Session& session) {
    if (!session.input.empty()) return narrow(session.input.stem().wstring());
    return "Nova transcrição";
}

constexpr const char* kQueueItemPayload = "STO_QUEUE_ITEM";
constexpr float kGroupChildIndent = 20.0F;

// Drag-drop payloads carry a variable-length array of QueueKey: a single item
// for a normal drag, or the whole multi-selection (in queue order) when
// dragging a session that is part of one.
std::vector<QueueKey> read_drag_payload(const ImGuiPayload* payload) {
    const auto* keys = static_cast<const QueueKey*>(payload->Data);
    const std::size_t count = static_cast<std::size_t>(payload->DataSize) / sizeof(QueueKey);
    return std::vector<QueueKey>(keys, keys + count);
}

void render_session_card(State& module, const std::shared_ptr<Session>& session, float indent) {
    const Status status = session->status.load();
    const ImVec4 color = session_color(status);
    const bool selected = module.is_session_selected(session->id);
    const std::string title = session_title(*session);
    const std::string short_title = ellipsize(title, indent > 0.0F ? 21 : 25);
    const ImVec2 size = {-1.0F, module.queue_sidebar_collapsed ? 38.0F : 62.0F};
    const std::string item_id = std::format("##session-card-{}", session->id);
    if (indent > 0.0F) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    ImGui::InvisibleButton(item_id.c_str(), size);
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    const bool ctrl_held = ImGui::GetIO().KeyCtrl;
    const bool shift_held = ImGui::GetIO().KeyShift;
    if (clicked) {
        // Clicking on a card that's already part of a multi-selection, with
        // no modifier, is ambiguous: it could be the start of a drag of the
        // whole selection, or a plain click meant to collapse the selection
        // to just this card. Defer the decision until release/drag below so
        // dragging an already-selected card moves the whole selection.
        if (!ctrl_held && !shift_held && selected && module.queue_selection.size() > 1) {
            module.pending_selection_click_id = session->id;
        } else {
            module.handle_session_click(session->id, ctrl_held, shift_held);
            module.pending_selection_click_id = 0;
        }
    }
    if (module.pending_selection_click_id == session->id) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            module.pending_selection_click_id = 0;
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            module.handle_session_click(session->id, false, false);
            module.pending_selection_click_id = 0;
        }
    }

    // Right-click opens a context menu for batch actions on the current
    // selection. Right-clicking outside the selection first selects just
    // this card, so the menu always acts on what's highlighted.
    const std::string context_id = std::format("session-context-{}", session->id);
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        if (!module.is_session_selected(session->id)) {
            module.handle_session_click(session->id, false, false);
        }
        ImGui::OpenPopup(context_id.c_str());
    }
    if (ImGui::BeginPopup(context_id.c_str())) {
        const std::size_t count = module.queue_selection.size();
        const std::string suffix = count > 1 ? std::format(" ({})", count) : "";
        if (ImGui::MenuItem((label(kIconClock, "Adicionar à fila") + suffix).c_str())) {
            module.request_queue_selection();
        }
        if (ImGui::MenuItem((label(kIconPause, "Pausar") + suffix).c_str())) {
            module.request_pause_selection();
        }
        ImGui::Separator();
        if (ImGui::MenuItem((label(kIconTrash, "Excluir") + suffix).c_str())) {
            module.bulk_delete_confirmation = true;
        }
        ImGui::EndPopup();
    }

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (indent > 0.0F) {
        draw->AddRectFilled({min.x - indent + 7.0F, min.y}, {min.x - indent + 9.0F, max.y}, ImGui::GetColorU32(ImVec4{1.0F, 1.0F, 1.0F, 0.08F}), 1.0F);
    }
    const ImU32 fill = ImGui::GetColorU32(selected ? ImVec4{0.16F, 0.17F, 0.18F, 1.0F} : hovered ? ImVec4{0.13F, 0.14F, 0.15F, 1.0F} : ImVec4{0.08F, 0.09F, 0.10F, 1.0F});
    draw->AddRectFilled(min, max, fill, 7.0F);
    draw->AddRect(min, max, ImGui::GetColorU32(color), 7.0F, 0, selected ? 2.0F : 1.0F);
    if (module.queue_sidebar_collapsed) {
        const ImVec2 icon_size = ImGui::CalcTextSize(session_icon(status));
        draw->AddText({min.x + ((max.x - min.x) - icon_size.x) * 0.5F, min.y + ((max.y - min.y) - icon_size.y) * 0.5F}, ImGui::GetColorU32(color), session_icon(status));
    } else {
        draw->AddText({min.x + 12.0F, min.y + 9.0F}, ImGui::GetColorU32(color), session_icon(status));
        draw->AddText({min.x + 36.0F, min.y + 9.0F}, ImGui::GetColorU32(kWhite), short_title.c_str());
        const std::string status_text = session_status_text(*session);
        const ImVec2 status_size = ImGui::CalcTextSize(status_text.c_str());
        draw->AddText({min.x + ((max.x - min.x) - status_size.x) * 0.5F, min.y + 36.0F}, ImGui::GetColorU32(color), status_text.c_str());
    }
    if (status != Status::running && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const std::vector<QueueKey> dragged = module.drag_payload_for(session->id);
        ImGui::SetDragDropPayload(kQueueItemPayload, dragged.data(), dragged.size() * sizeof(QueueKey));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.0F, 10.0F});
        ImGui::BeginGroup();
        if (dragged.size() > 1) {
            ImGui::TextColored(kWhite, "%s", std::format("{} oitivas selecionadas", dragged.size()).c_str());
        } else {
            ImGui::TextColored(color, "%s", session_icon(status));
            ImGui::SameLine();
            ImGui::TextColored(kWhite, "%s", short_title.c_str());
            ImGui::TextColored(color, "%s", session_status_text(*session).c_str());
        }
        ImGui::EndGroup();
        ImGui::PopStyleVar();
        ImGui::EndDragDropSource();
    }
    if (status != Status::running && ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* preview_payload = ImGui::AcceptDragDropPayload(kQueueItemPayload, ImGuiDragDropFlags_AcceptPeekOnly);
        const bool insert_after = ImGui::GetMousePos().y > (min.y + max.y) * 0.5F;
        if (preview_payload != nullptr) {
            const float y = insert_after ? max.y + 2.0F : min.y - 2.0F;
            draw->AddRectFilled({min.x + 4.0F, y - 2.0F}, {max.x - 4.0F, y + 2.0F}, ImGui::GetColorU32(kBlue), 2.0F);
            draw->AddRectFilled({min.x + 8.0F, min.y + 4.0F}, {max.x - 8.0F, max.y - 4.0F}, ImGui::GetColorU32(ImVec4{0.00F, 0.48F, 1.00F, 0.08F}), 7.0F);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kQueueItemPayload)) {
            module.move_queue_items(read_drag_payload(payload), QueueKey{false, session->id}, insert_after);
        }
        ImGui::EndDragDropTarget();
    }
    if (hovered) ImGui::SetTooltip("%s\n%s", title.c_str(), session_status_text(*session).c_str());
    ImGui::Spacing();
}

// Renders a separator's header (collapse toggle, folder icon, name and a
// small item count), styled to blend in with the rest of the queue rather than
// stand out. Adding/renaming/removing separators happens entirely through the
// right-click context menus (on the queue background and on this header), and
// placing sessions in/out of the separator happens by dropping them onto this
// header, onto one of its children, or onto the empty queue background to
// move them back to the top level — there is no dedicated drop zone.
void render_group_section(State& module, const std::shared_ptr<Group>& group, const std::vector<std::shared_ptr<Session>>& sessions_snapshot) {
    std::vector<QueueKey> children;
    {
        std::scoped_lock lock(module.sessions_mutex);
        children = module.container_items_unlocked(group->id);
    }
    bool has_running = false;
    int count = 0;
    for (const auto& key : children) {
        const auto it = std::find_if(sessions_snapshot.begin(), sessions_snapshot.end(), [&](const auto& session) {
            return session->id == key.id;
        });
        if (it == sessions_snapshot.end()) continue;
        ++count;
        if ((*it)->status.load() == Status::running) has_running = true;
    }

    // Keep the active transcription pinned to the top within its separator too.
    if (has_running) {
        std::stable_sort(children.begin(), children.end(), [&](const QueueKey& left, const QueueKey& right) {
            const auto left_it = std::find_if(sessions_snapshot.begin(), sessions_snapshot.end(), [&](const auto& session) {
                return session->id == left.id;
            });
            const auto right_it = std::find_if(sessions_snapshot.begin(), sessions_snapshot.end(), [&](const auto& session) {
                return session->id == right.id;
            });
            const bool left_running = left_it != sessions_snapshot.end() && (*left_it)->status.load() == Status::running;
            const bool right_running = right_it != sessions_snapshot.end() && (*right_it)->status.load() == Status::running;
            return left_running && !right_running;
        });
    }

    constexpr float header_height = 32.0F;
    ImGui::SetNextItemAllowOverlap();
    const std::string header_id = std::format("##group-header-{}", group->id);
    ImGui::InvisibleButton(header_id.c_str(), {-1.0F, header_height});
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (hovered) draw->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4{1.0F, 1.0F, 1.0F, 0.03F}), 5.0F);

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const QueueKey payload{true, group->id};
        ImGui::SetDragDropPayload(kQueueItemPayload, &payload, sizeof(payload));
        ImGui::TextColored(kSilver, "%s  %s", kIconFolder, group->name.data());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* preview = ImGui::AcceptDragDropPayload(kQueueItemPayload, ImGuiDragDropFlags_AcceptPeekOnly);
        if (preview != nullptr) {
            draw->AddRect(min, max, ImGui::GetColorU32(ImVec4{1.0F, 1.0F, 1.0F, 0.20F}), 5.0F, 0, 1.0F);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kQueueItemPayload)) {
            const bool insert_after = ImGui::GetMousePos().y > (min.y + max.y) * 0.5F;
            module.move_queue_items(read_drag_payload(payload), QueueKey{true, group->id}, insert_after);
        }
        ImGui::EndDragDropTarget();
    }

    // Right-click anywhere on the header opens the context menu. A plain
    // geometric hit-test (rather than BeginPopupContextItem on the
    // InvisibleButton) is used because the toggle button and name field are
    // drawn on top of it with AllowOverlap, which would otherwise steal the
    // hover/click for parts of the header.
    const std::string context_id = std::format("group-context-{}", group->id);
    if (ImGui::IsMouseHoveringRect(min, max) && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(context_id.c_str());
    }
    if (ImGui::BeginPopup(context_id.c_str())) {
        if (ImGui::MenuItem("Renomear separador")) {
            module.group_rename_target_id = group->id;
            module.group_rename_focus_pending = true;
        }
        if (ImGui::MenuItem("Excluir separador")) {
            module.delete_group_confirmation = true;
            module.delete_group_target_id = group->id;
        }
        ImGui::EndPopup();
    }

    const float center_y = min.y + (max.y - min.y) * 0.5F;
    constexpr float toggle_x = 4.0F;
    constexpr float toggle_size = 22.0F;
    constexpr float icon_x = toggle_x + toggle_size + 12.0F;
    constexpr float name_x = icon_x + 26.0F;

    ImGui::SetNextItemAllowOverlap();
    ImGui::SetCursorScreenPos({min.x + toggle_x, center_y - toggle_size * 0.5F});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1.0F, 1.0F, 1.0F, 0.07F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{1.0F, 1.0F, 1.0F, 0.12F});
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    const std::string toggle_id = std::format("{}##group-toggle-{}", group->collapsed ? kIconChevronRight : kIconChevronDown, group->id);
    if (ImGui::Button(toggle_id.c_str(), {toggle_size, toggle_size})) {
        group->collapsed = !group->collapsed;
        module.save_group(group);
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(group->collapsed ? "Expandir separador" : "Recolher separador");

    draw->AddText({min.x + icon_x, center_y - ImGui::GetFontSize() * 0.5F}, ImGui::GetColorU32(kSilver), kIconFolder);

    const std::string count_text = std::format("({})", count);
    const ImVec2 count_size = ImGui::CalcTextSize(count_text.c_str());
    const float right_reserved = count_size.x + 10.0F;
    const float name_width = std::max(40.0F, (max.x - min.x) - name_x - right_reserved);
    const std::string name_id = std::format("##group-name-{}", group->id);
    if (module.group_rename_target_id == group->id) {
        ImGui::SetNextItemAllowOverlap();
        ImGui::SetCursorScreenPos({min.x + name_x, center_y - ImGui::GetFrameHeight() * 0.5F});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{1.0F, 1.0F, 1.0F, 0.06F});
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{1.0F, 1.0F, 1.0F, 0.08F});
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4{1.0F, 1.0F, 1.0F, 0.10F});
        ImGui::PushStyleColor(ImGuiCol_Text, kSilver);
        ImGui::SetNextItemWidth(name_width);
        if (module.group_rename_focus_pending) {
            ImGui::SetKeyboardFocusHere();
            module.group_rename_focus_pending = false;
        }
        ImGui::InputText(name_id.c_str(), group->name.data(), group->name.size());
        if (ImGui::IsItemDeactivated()) {
            module.save_group(group);
            module.group_rename_target_id = 0;
        }
        ImGui::PopStyleColor(4);
    } else {
        const std::string display_name = ellipsize(std::string(group->name.data()), 26);
        draw->AddText({min.x + name_x, center_y - ImGui::GetFontSize() * 0.5F}, ImGui::GetColorU32(kSilver), display_name.c_str());
    }

    draw->AddText({max.x - count_size.x, center_y - count_size.y * 0.5F}, ImGui::GetColorU32(kMuted), count_text.c_str());
    if (group->collapsed && has_running) {
        draw->AddCircleFilled({max.x - count_size.x - 10.0F, center_y}, 4.0F, ImGui::GetColorU32(kBlue));
    }

    ImGui::SetCursorScreenPos({min.x, max.y});
    ImGui::Spacing();

    if (group->collapsed) return;

    if (children.empty()) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kGroupChildIndent);
        ImGui::TextColored(kMuted, "Nenhuma oitiva");
        ImGui::Spacing();
    } else {
        for (const auto& key : children) {
            const auto it = std::find_if(sessions_snapshot.begin(), sessions_snapshot.end(), [&](const auto& session) {
                return session->id == key.id;
            });
            if (it == sessions_snapshot.end()) continue;
            render_session_card(module, *it, kGroupChildIndent);
        }
    }
}

void render_queue_sidebar(State& module) {
    constexpr ImGuiWindowFlags sidebar_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const float sidebar_width = module.queue_sidebar_collapsed ? 58.0F : 282.0F;
    ImGui::BeginChild("transcription-queue-sidebar", {sidebar_width, 0.0F}, ImGuiChildFlags_Borders, sidebar_flags);

    const char* collapse_icon = module.queue_sidebar_collapsed ? kIconChevronLeft : kIconChevronRight;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 38.0F);
    if (ImGui::Button(collapse_icon, {28.0F, 28.0F})) module.queue_sidebar_collapsed = !module.queue_sidebar_collapsed;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(module.queue_sidebar_collapsed ? "Expandir fila" : "Recolher fila");

    if (!module.queue_sidebar_collapsed) {
        ImGui::TextColored(kSilver, "FILA DE TRANSCRIÇÕES");
    }
    ImGui::Spacing();
    if (colored_button(
        module.queue_sidebar_collapsed ? std::string(kIconPlus) + "##new-session" : label(kIconPlus, "Nova transcrição"),
        {-1.0F, 38.0F},
        kBlue
    )) module.create_session();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Criar nova transcrição");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginChild("transcription-queue-list", {0.0F, 0.0F}, false);

    std::vector<std::shared_ptr<Session>> sessions_snapshot;
    std::vector<std::shared_ptr<Group>> groups_snapshot;
    std::vector<QueueKey> top_level;
    std::vector<std::pair<long long, bool>> group_has_running;
    {
        std::scoped_lock lock(module.sessions_mutex);
        sessions_snapshot = module.sessions;
        groups_snapshot = module.groups;
        top_level = module.container_items_unlocked(0);
        for (const auto& group : groups_snapshot) {
            bool has_running = false;
            for (const auto& key : module.container_items_unlocked(group->id)) {
                const auto it = std::find_if(sessions_snapshot.begin(), sessions_snapshot.end(), [&](const auto& session) {
                    return session->id == key.id;
                });
                if (it != sessions_snapshot.end() && (*it)->status.load() == Status::running) {
                    has_running = true;
                    break;
                }
            }
            group_has_running.emplace_back(group->id, has_running);
        }
    }

    if (module.queue_sidebar_collapsed) {
        std::stable_sort(sessions_snapshot.begin(), sessions_snapshot.end(), [](const auto& left, const auto& right) {
            const bool left_running = left->status == Status::running;
            const bool right_running = right->status == Status::running;
            if (left_running != right_running) return left_running;
            return left->queue_order < right->queue_order;
        });
        for (const auto& session : sessions_snapshot) render_session_card(module, session, 0.0F);
    } else {
        // Keep the active transcription pinned to the top of the queue,
        // regardless of its saved position, since the queue reads
        // top-to-bottom as "what's happening now, then what's next".
        const auto key_has_running = [&](const QueueKey& key) {
            if (key.is_group) {
                const auto it = std::find_if(group_has_running.begin(), group_has_running.end(), [&](const auto& entry) {
                    return entry.first == key.id;
                });
                return it != group_has_running.end() && it->second;
            }
            const auto it = std::find_if(sessions_snapshot.begin(), sessions_snapshot.end(), [&](const auto& session) {
                return session->id == key.id;
            });
            return it != sessions_snapshot.end() && (*it)->status.load() == Status::running;
        };
        std::stable_sort(top_level.begin(), top_level.end(), [&](const QueueKey& left, const QueueKey& right) {
            return key_has_running(left) && !key_has_running(right);
        });

        for (const auto& key : top_level) {
            if (key.is_group) {
                const auto group_it = std::find_if(groups_snapshot.begin(), groups_snapshot.end(), [&](const auto& group) {
                    return group->id == key.id;
                });
                if (group_it == groups_snapshot.end()) continue;
                render_group_section(module, *group_it, sessions_snapshot);
            } else {
                const auto session_it = std::find_if(sessions_snapshot.begin(), sessions_snapshot.end(), [&](const auto& session) {
                    return session->id == key.id;
                });
                if (session_it == sessions_snapshot.end()) continue;
                render_session_card(module, *session_it, 0.0F);
            }
        }

        // Empty space below the items: dropping a session here moves it back
        // to the top-level queue (out of any separator), and right-clicking
        // here opens the "Novo separador" menu.
        const float remaining_height = std::max(24.0F, ImGui::GetContentRegionAvail().y);
        ImGui::InvisibleButton("##queue-empty-area", {-1.0F, remaining_height});
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kQueueItemPayload)) {
                for (const QueueKey& moved : read_drag_payload(payload)) {
                    if (!moved.is_group) module.move_to_top_level_end(moved.id);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##queue-context")) {
            if (ImGui::MenuItem("Novo separador")) module.create_group();
            ImGui::EndPopup();
        }
    }

    ImGui::EndChild();
    ImGui::EndChild();

    if (module.delete_group_confirmation) ImGui::OpenPopup("Excluir separador");
    if (ImGui::BeginPopupModal("Excluir separador", &module.delete_group_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("As oitivas deste separador voltarão para a fila principal.");
        ImGui::TextColored(kSilver, "O separador será removido, mas nada será excluído.");
        ImGui::Spacing();
        if (ImGui::Button("Cancelar", {120.0F, 36.0F})) module.delete_group_confirmation = false;
        ImGui::SameLine();
        if (colored_button(label(kIconTrash, "Excluir separador"), {190.0F, 36.0F}, kRed)) {
            module.delete_group(module.delete_group_target_id);
            module.delete_group_confirmation = false;
        }
        ImGui::EndPopup();
    }

    if (module.bulk_delete_confirmation) ImGui::OpenPopup("Excluir transcrições");
    if (ImGui::BeginPopupModal("Excluir transcrições", &module.bulk_delete_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int count = static_cast<int>(module.queue_selection.size());
        if (count > 1) ImGui::Text("Deseja realmente excluir as %d transcrições selecionadas?", count);
        else ImGui::Text("Deseja realmente excluir a transcrição selecionada?");
        ImGui::TextColored(kSilver, "Transcrições em andamento não serão removidas.");
        ImGui::Spacing();
        if (ImGui::Button("Cancelar", {120.0F, 36.0F})) module.bulk_delete_confirmation = false;
        ImGui::SameLine();
        if (colored_button(label(kIconTrash, "Excluir"), {150.0F, 36.0F}, kRed)) {
            module.remove_queue_selection();
            module.bulk_delete_confirmation = false;
        }
        ImGui::EndPopup();
    }

    if (module.pause_running_confirmation) ImGui::OpenPopup("Confirmar pausa");
    if (ImGui::BeginPopupModal("Confirmar pausa", &module.pause_running_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Uma transcrição da seleção está em andamento.");
        ImGui::TextColored(kSilver, "Cancelar a transcrição atual e marcar como pausada?");
        ImGui::Spacing();
        if (ImGui::Button("Continuar transcrevendo", {190.0F, 36.0F})) module.pause_running_confirmation = false;
        ImGui::SameLine();
        if (colored_button("Cancelar e pausar", {160.0F, 36.0F}, kYellow, kDarkText)) {
            module.confirm_pause_running();
        }
        ImGui::EndPopup();
    }

    if (module.requeue_overwrite_confirmation) ImGui::OpenPopup("Confirmar substituição");
    if (ImGui::BeginPopupModal("Confirmar substituição", &module.requeue_overwrite_confirmation, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Uma ou mais transcrições selecionadas já possuem texto transcrito.");
        ImGui::TextColored(kSilver, "Adicionar à fila substituirá esse conteúdo ao retranscrever.");
        ImGui::Spacing();
        if (ImGui::Button("Cancelar", {120.0F, 36.0F})) module.requeue_overwrite_confirmation = false;
        ImGui::SameLine();
        if (colored_button(label(kIconClock, "Adicionar à fila"), {170.0F, 36.0F}, kBlue)) {
            module.set_selection_status(Status::queued);
            module.requeue_overwrite_confirmation = false;
        }
        ImGui::EndPopup();
    }
}

void render_inline_notification(State& module) {
    const auto [text, kind, started] = module.get_note();
    if (text.empty()) return;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (elapsed > 4.0) return;
    const float alpha = static_cast<float>(std::clamp(1.0 - std::max(0.0, elapsed - 3.2) / 0.8, 0.0, 1.0));
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 window_position = viewport->WorkPos;
    const ImVec2 window_size = viewport->WorkSize;
    const ImVec4 accent = kind == NoteKind::error ? kError : kind == NoteKind::warning ? kYellow : kSuccess;
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
        module.clear_note();
        return;
    }
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(position, end, ImGui::GetColorU32(ImVec4{0.04F, 0.05F, 0.06F, 0.88F * alpha}), 8.0F);
    draw->AddRect(position, end, ImGui::GetColorU32(faded_accent), 8.0F, 0, 1.2F);
    draw->AddText({position.x + (width - text_size.x) * 0.5F, position.y + (height - text_size.y) * 0.5F}, ImGui::GetColorU32(faded_accent), text.c_str());
}

}

void render() {
    State& module = state();
    constexpr float horizontal_margin = 10.0F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + horizontal_margin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0F, 8.0F});
    ImGui::BeginChild(
        "transcription-module-frame",
        {ImGui::GetContentRegionAvail().x - horizontal_margin, 0.0F},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    ImGui::TextColored(kSilver, "MÓDULOS  /  TRANSCRIÇÃO DE OITIVAS");
    ImGui::Spacing();
    if (sto::ui::heading_font()) ImGui::PushFont(sto::ui::heading_font());
    constexpr const char* title = "Transcrição de Oitivas";
    const float title_width = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - title_width) * 0.5F));
    ImGui::TextColored(kWhite, "%s", title);
    if (sto::ui::heading_font()) ImGui::PopFont();
    ImGui::Separator();
    if (module.database_ready) {
        module.start_queue_worker();
        const float queue_width = module.queue_sidebar_collapsed ? 58.0F : 282.0F;
        ImGui::BeginChild("transcription-station-area", {ImGui::GetContentRegionAvail().x - queue_width - ImGui::GetStyle().ItemSpacing.x, 0.0F}, false, ImGuiWindowFlags_NoScrollbar);
        if (module.selected) render_station(module, *module.selected);
        ImGui::EndChild();
        ImGui::SameLine();
        render_queue_sidebar(module);
        render_inline_notification(module);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void render_prompt_settings() {
    State& module = state();
    constexpr float editor_height = 180.0F;
    constexpr std::array kPromptTypes = {"Contexto", "Procedimento Tradicional", "Procedimento Formal"};
    ImGui::TextColored(kMuted, "Prompt a editar");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::Combo("##settings-editor-procedure", &module.editor_procedure, kPromptTypes.data(), static_cast<int>(kPromptTypes.size()));
    ImGui::Spacing();
    auto& editor = module.editor_procedure == 0 ? module.editor_context
                 : module.editor_procedure == 1 ? module.editor_traditional
                 : module.editor_formal;
    ImGui::InputTextMultiline(
        "##settings-prompt-editor",
        &editor,
        {-1.0F, editor_height},
        ImGuiInputTextFlags_WordWrap
    );
    ImGui::Spacing();
    if (colored_button(label(kIconSave, "Salvar configurações"), {-1.0F, 38.0F}, kGreen)) {
        module.prompts = {module.editor_context, module.editor_traditional, module.editor_formal};
        std::string error;
        const bool saved = module.repository.save_prompts(module.prompts, error);
        module.set_note(
            saved ? "Configurações salvas com sucesso." : "Não foi possível salvar: " + error,
            saved ? NoteKind::success : NoteKind::error
        );
    }
}

void render_notification() {
    render_inline_notification(state());
}

SessionSnapshot get_selected_session_snapshot() {
    State& module = state();
    SessionSnapshot snap;
    if (!module.selected) return snap;
    snap.has_session = true;
    snap.session_id = module.selected->id;
    snap.hearing_type = module.selected->hearing_type;
    snap.party_name = module.selected->party_name.data();
    snap.date_oitiva = module.selected->data_oitiva.data();
    return snap;
}
}
