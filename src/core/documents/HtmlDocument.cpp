#include "core/documents/HtmlDocument.hpp"

#include <windows.h>
#include <shellapi.h>

#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace sto::documents {
namespace {

bool read_file_bytes(const std::filesystem::path& path, std::vector<unsigned char>& bytes, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Não foi possível abrir o arquivo: " + path.string();
        return false;
    }

    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool read_text_file(const std::filesystem::path& path, std::string& text, std::string& error) {
    std::vector<unsigned char> bytes;
    if (!read_file_bytes(path, bytes, error)) return false;
    text.assign(bytes.begin(), bytes.end());
    return true;
}

std::string print_button_script() {
    return R"HTML(
    (function () {
      function injectScreenExtras() {
        if (!document.querySelector(".pagedjs_pages")) {
          setTimeout(injectScreenExtras, 50);
          return;
        }
        const style = document.createElement("style");
        style.textContent = "@media screen {"
          + "body { background: #d6d9dc; }"
          + ".pagedjs_page { margin: 12mm auto; box-shadow: 0 2px 10px rgba(0, 0, 0, 0.25); }"
          + ".pagedjs_sheet { background: #fff; }"
          + "}"
          + ".pagedjs_margin-bottom { position: relative !important; }"
          + ".pagedjs_margin-bottom-left { display: none !important; }"
          + ".pagedjs_margin-bottom-center { position: absolute !important; left: -25mm !important; width: 210mm !important; bottom: 5.5mm !important; text-align: center !important; }"
          + ".pagedjs_margin-bottom-right { position: absolute !important; right: 0 !important; bottom: 18mm !important; width: auto !important; text-align: right !important; }"
          + ".sto-print-button {"
          + "position: fixed; bottom: 24px; right: 24px; width: 56px; height: 56px;"
          + "border-radius: 50%; border: none; background: #0d6efd; color: #fff;"
          + "display: flex; align-items: center; justify-content: center; cursor: pointer;"
          + "box-shadow: 0 2px 10px rgba(0, 0, 0, 0.35); z-index: 1000;"
          + "}"
          + ".sto-print-button:hover { background: #0b5ed7; }"
          + "@media print { .sto-print-button { display: none; } }";
        document.head.appendChild(style);

        const button = document.createElement("button");
        button.className = "sto-print-button";
        button.title = "Imprimir ou salvar como PDF";
        button.innerHTML = '<svg viewBox="0 0 24 24" width="26" height="26" fill="currentColor">'
          + '<path d="M19 8H5c-1.66 0-3 1.34-3 3v6h4v4h12v-4h4v-6c0-1.66-1.34-3-3-3zm-3 11H8v-5h8v5zm3-7c-.55 0-1-.45-1-1s.45-1 1-1 1 .45 1 1-.45 1-1 1zm-1-9H6v4h12V3z"/>'
          + '</svg>';
        button.addEventListener("click", function () { window.print(); });
        document.body.appendChild(button);
      }
      injectScreenExtras();
    })();
)HTML";
}

} // namespace

std::string escape_html(std::string_view text) {
    std::string output;
    output.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&': output += "&amp;"; break;
        case '<': output += "&lt;"; break;
        case '>': output += "&gt;"; break;
        case '"': output += "&quot;"; break;
        default: output += character; break;
        }
    }
    return output;
}

std::string paragraphs_from_wrapped_text(std::string_view text, std::string_view indentation) {
    std::string html;
    std::istringstream input{std::string(text)};
    std::string paragraph;
    std::string line;

    const auto flush = [&] {
        if (paragraph.empty()) return;
        html += indentation;
        html += "<p>";
        html += escape_html(paragraph);
        html += "</p>\n";
        paragraph.clear();
    };

    while (std::getline(input, line)) {
        if (line.empty()) {
            flush();
        } else {
            if (!paragraph.empty()) paragraph += ' ';
            paragraph += line;
        }
    }
    flush();

    if (html.empty()) {
        html += indentation;
        html += "<p></p>\n";
    }
    return html;
}

std::string base64_encode(const std::vector<unsigned char>& data) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((data.size() + 2) / 3) * 4);
    std::size_t index = 0;
    for (; index + 3 <= data.size(); index += 3) {
        const unsigned int chunk = (static_cast<unsigned int>(data[index]) << 16)
            | (static_cast<unsigned int>(data[index + 1]) << 8)
            | static_cast<unsigned int>(data[index + 2]);
        output += table[(chunk >> 18) & 0x3F];
        output += table[(chunk >> 12) & 0x3F];
        output += table[(chunk >> 6) & 0x3F];
        output += table[chunk & 0x3F];
    }

    const std::size_t remaining = data.size() - index;
    if (remaining == 1) {
        const unsigned int chunk = static_cast<unsigned int>(data[index]) << 16;
        output += table[(chunk >> 18) & 0x3F];
        output += table[(chunk >> 12) & 0x3F];
        output += "==";
    } else if (remaining == 2) {
        const unsigned int chunk = (static_cast<unsigned int>(data[index]) << 16)
            | (static_cast<unsigned int>(data[index + 1]) << 8);
        output += table[(chunk >> 18) & 0x3F];
        output += table[(chunk >> 12) & 0x3F];
        output += table[(chunk >> 6) & 0x3F];
        output += "=";
    }
    return output;
}

bool load_default_assets(const std::filesystem::path& assets_directory, DocumentAssets& assets, std::string& error) {
    std::vector<unsigned char> logo_bytes;
    if (!read_file_bytes(assets_directory / "logo_pcsc.png", logo_bytes, error)) return false;
    assets.logo_base64 = base64_encode(logo_bytes);

    if (!read_text_file(assets_directory / "paged.polyfill.js", assets.paged_js, error)) return false;
    return true;
}

std::string build_paged_html(const PagedDocument& document, const DocumentAssets& assets) {
    std::string html = R"HTML(<!doctype html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <title>)HTML";
    html += escape_html(document.browser_title.empty() ? document.document_title : document.browser_title);
    html += R"HTML(</title>
  <script>
    window.requestAnimationFrame = function (callback) {
      return setTimeout(function () { callback(performance.now()); }, 0);
    };
    window.requestIdleCallback = window.requestIdleCallback || function (callback) {
      return setTimeout(function () {
        callback({ didTimeout: false, timeRemaining: function () { return 50; } });
      }, 0);
    };
  </script>
  <script>
)HTML";
    html += assets.paged_js;
    html += R"HTML(
  </script>
  <style>
    * { box-sizing: border-box; }
    html, body { margin: 0; padding: 0; }
    body { font-family: Arial, "Liberation Sans", sans-serif; color: #000; font-size: 11pt; line-height: 12pt; }
    p { margin: 0; padding: 0; }
    .justified { text-align: justify; text-indent: 15mm; margin-bottom: 2mm; }

    @page {
      size: A4;
      margin-top: 48mm;
      margin-right: 20mm;
      margin-bottom: 28mm;
      margin-left: 25mm;

      @top-center { content: element(running-header); }
      @bottom-center { content: element(running-footer); }
      @bottom-right {
        content: "Página " counter(page) " de " counter(pages);
        font-size: 9pt;
        font-weight: 700;
      }
    }

    .page-header {
      position: running(running-header);
      width: 100%;
      text-align: center;
      font-size: 12pt;
      line-height: 12.4pt;
    }
    .page-header img {
      display: block;
      width: 19mm;
      height: 25mm;
      object-fit: contain;
      margin: 0 auto 1.8mm auto;
    }
    .page-header .line { display: block; }

    .page-footer {
      position: running(running-footer);
      width: 100%;
      text-align: center;
      font-size: 9pt;
      line-height: 10pt;
    }
    .page-footer b { font-weight: 700; }

    .title {
      text-align: center;
      font-size: 11pt;
      line-height: 12pt;
      font-weight: 700;
      text-decoration: underline;
      margin-bottom: 4mm;
    }
)HTML";
    html += document.extra_style;
    html += document.custom_css;
    html += R"HTML(
  </style>
  <script>
)HTML";
    html += print_button_script();
    html += R"HTML(
  </script>
</head>
<body>
  <div class="page-header">
    <img src="data:image/png;base64,)HTML";
    html += assets.logo_base64;
    html += R"HTML(" alt=")HTML";
    html += escape_html(document.logo_alt);
    html += R"HTML(">
)HTML";

    if (!document.header_html.empty()) {
        html += document.header_html;
    } else {
        for (const std::string& line : document.header_lines) {
            html += "    <span class=\"line\">";
            html += escape_html(line);
            html += "</span>\n";
        }
    }

    html += R"HTML(  </div>
  <div class="page-footer">
)HTML";
    html += document.footer_html;
    html += R"HTML(  </div>

  <div class="title">)HTML";
    html += escape_html(document.document_title);
    html += R"HTML(</div>

)HTML";
    html += document.body_html;
    html += R"HTML(</body>
</html>
)HTML";

    return html;
}

std::filesystem::path temporary_directory() {
    return std::filesystem::temp_directory_path() / "sto_documents";
}

bool write_html_file(const std::filesystem::path& path, std::string_view html, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Não foi possível criar a pasta temporária: " + ec.message();
        return false;
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "Não foi possível criar o arquivo HTML.";
        return false;
    }

    file.write(html.data(), static_cast<std::streamsize>(html.size()));
    if (!file) {
        error = "Não foi possível gravar o arquivo HTML.";
        return false;
    }
    return true;
}

bool open_in_default_browser(const std::filesystem::path& path, std::string& error) {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result > 32) return true;
    error = "Não foi possível abrir o navegador padrão.";
    return false;
}

std::vector<std::string> default_pcsc_header_lines() {
    return {};
}

std::string default_pcsc_footer_html() {
    return {};
}

}
