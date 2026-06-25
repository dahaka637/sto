#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sto::documents {

struct DocumentAssets {
    std::string logo_base64;
    std::string paged_js;
};

struct PagedDocument {
    std::string browser_title;
    std::string document_title;
    std::vector<std::string> header_lines; // used only if header_html is empty
    std::string header_html;  // raw HTML inside the header div (overrides header_lines)
    std::string footer_html;
    std::string body_html;
    std::string extra_style;
    std::string custom_css;   // additional user CSS appended after extra_style
    std::string logo_alt = "";
};

std::string escape_html(std::string_view text);
std::string paragraphs_from_wrapped_text(std::string_view text, std::string_view indentation = "    ");
std::string base64_encode(const std::vector<unsigned char>& data);

bool load_default_assets(const std::filesystem::path& assets_directory, DocumentAssets& assets, std::string& error);
std::string build_paged_html(const PagedDocument& document, const DocumentAssets& assets);

std::filesystem::path temporary_directory();
bool write_html_file(const std::filesystem::path& path, std::string_view html, std::string& error);
bool open_in_default_browser(const std::filesystem::path& path, std::string& error);

std::vector<std::string> default_pcsc_header_lines();
std::string default_pcsc_footer_html();

}
