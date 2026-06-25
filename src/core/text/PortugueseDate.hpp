#pragma once

#include "imgui.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

namespace sto::text {

inline std::string month_name_pt(unsigned month) {
    static constexpr std::array<const char*, 12> names = {
        "janeiro", "fevereiro", "março", "abril", "maio", "junho",
        "julho", "agosto", "setembro", "outubro", "novembro", "dezembro"
    };
    if (month < 1 || month > 12) return "";
    return names[month - 1];
}

namespace detail {

// Converts a number from 0 to 999 to its Portuguese cardinal form.
inline std::string numero_extenso_pt(int n) {
    static constexpr std::array<const char*, 10> units = {
        "zero", "um", "dois", "três", "quatro", "cinco", "seis", "sete", "oito", "nove"
    };
    static constexpr std::array<const char*, 10> teens = {
        "dez", "onze", "doze", "treze", "catorze", "quinze",
        "dezesseis", "dezessete", "dezoito", "dezenove"
    };
    static constexpr std::array<const char*, 10> tens = {
        "", "", "vinte", "trinta", "quarenta", "cinquenta",
        "sessenta", "setenta", "oitenta", "noventa"
    };
    static constexpr std::array<const char*, 10> hundreds = {
        "", "cento", "duzentos", "trezentos", "quatrocentos", "quinhentos",
        "seiscentos", "setecentos", "oitocentos", "novecentos"
    };

    if (n <= 0) return units[0];
    if (n < 10) return units[n];
    if (n < 20) return teens[n - 10];

    std::string result;
    const int hundred_digit = n / 100;
    const int remainder = n % 100;

    if (hundred_digit > 0) {
        result = (n == 100) ? "cem" : hundreds[hundred_digit];
    }

    if (remainder > 0) {
        std::string tail;
        if (remainder < 10) {
            tail = units[remainder];
        } else if (remainder < 20) {
            tail = teens[remainder - 10];
        } else {
            tail = tens[remainder / 10];
            const int unit_digit = remainder % 10;
            if (unit_digit > 0) {
                tail += " e ";
                tail += units[unit_digit];
            }
        }

        if (!result.empty()) result += " e ";
        result += tail;
    }

    return result;
}

}

// Converts a year to its Portuguese cardinal form, e.g. 2024 -> "dois mil e
// vinte e quatro", 1998 -> "mil novecentos e noventa e oito".
inline std::string year_extenso_pt(int year) {
    if (year <= 0) return detail::numero_extenso_pt(year);

    const int thousands = year / 1000;
    const int remainder = year % 1000;

    std::string result = (thousands == 1) ? "mil" : (detail::numero_extenso_pt(thousands) + " mil");

    if (remainder > 0) {
        result += (remainder < 100) ? " e " : " ";
        result += detail::numero_extenso_pt(remainder);
    }

    return result;
}

// Formats a date as "09 dias do mês de agosto do ano de dois mil e vinte e quatro".
inline std::string format_data_extenso(int day, unsigned month, int year) {
    char day_text[3]{};
    std::snprintf(day_text, sizeof(day_text), "%02d", day);
    return std::string(day_text) + " dias do mês de " + month_name_pt(month)
        + " do ano de " + year_extenso_pt(year);
}

// Parses a "DD/MM/YYYY" string into a calendar date, returning std::nullopt if
// the text is incomplete or does not represent a valid date.
inline std::optional<std::chrono::year_month_day> parse_date_ddmmyyyy(const std::string& text) {
    unsigned day = 0;
    unsigned month = 0;
    int year = 0;
    if (text.size() != 10 || text[2] != '/' || text[5] != '/') return std::nullopt;
    if (::sscanf_s(text.c_str(), "%2u/%2u/%4d", &day, &month, &year) != 3) return std::nullopt;

    const std::chrono::year_month_day date{
        std::chrono::year{year},
        std::chrono::month{month},
        std::chrono::day{day}
    };
    if (!date.ok()) return std::nullopt;
    return date;
}

// ImGuiInputTextFlags_CallbackEdit handler that reformats the buffer to the
// "DD/MM/YYYY" mask as the user types, keeping only digits and inserting the
// separators automatically.
inline int date_mask_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit) return 0;

    std::string digits;
    int cursor_digits = 0;
    for (int i = 0; i < data->BufTextLen; ++i) {
        const char c = data->Buf[i];
        if (c < '0' || c > '9') continue;
        if (digits.size() == 8) break;
        digits.push_back(c);
        if (i < data->CursorPos) ++cursor_digits;
    }

    std::string formatted;
    int new_cursor = 0;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i == 2 || i == 4) formatted.push_back('/');
        formatted.push_back(digits[i]);
        if (static_cast<int>(i) < cursor_digits) new_cursor = static_cast<int>(formatted.size());
    }

    const std::string original(data->Buf, static_cast<std::size_t>(data->BufTextLen));
    if (formatted == original) return 0;

    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, formatted.c_str());
    data->CursorPos = new_cursor;
    data->SelectionStart = data->CursorPos;
    data->SelectionEnd = data->CursorPos;
    return 0;
}

}
