#pragma once

#include "imgui.h"

#include <sstream>
#include <string>

namespace sto::ui {

// Extra breathing room reserved on the right edge when word-wrapping text
// displayed in read-only frames, so wrapped lines don't end flush against
// the border — mirroring the gap FramePadding already gives the left edge.
constexpr float kWrapRightMargin = 12.0F;

// Joins the whitespace-separated words of `paragraph` (embedded newlines are
// treated the same as spaces) and re-wraps them into lines of at most
// `width` pixels, breaking only at word boundaries.
inline std::string reflow_paragraph(const std::string& paragraph, float width) {
    std::istringstream words(paragraph);
    std::string result;
    std::string current;
    std::string word;
    while (words >> word) {
        const std::string candidate = current.empty() ? word : current + " " + word;
        if (!current.empty() && ImGui::CalcTextSize(candidate.c_str()).x > width) {
            if (!result.empty()) result += '\n';
            result += current;
            current = word;
        } else {
            current = candidate;
        }
    }
    if (!current.empty()) {
        if (!result.empty()) result += '\n';
        result += current;
    }
    return result;
}

// Re-wraps `text` so that no line exceeds `width` when measured with the
// current ImGui font, breaking on word boundaries while preserving blank
// lines between paragraphs.
inline std::string wrap_text(const std::string& text, float width) {
    std::istringstream input(text);
    std::string output;
    std::string paragraph;
    std::string line;

    while (std::getline(input, line)) {
        if (line.empty()) {
            if (!paragraph.empty()) {
                if (!output.empty()) output += '\n';
                output += reflow_paragraph(paragraph, width);
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
        output += reflow_paragraph(paragraph, width);
    }
    return output;
}

}
