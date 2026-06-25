#include "core/documents/HtmlDocument.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
}

int main() {
    if (!expect(sto::documents::escape_html("A & B < C > \"D\"") == "A &amp; B &lt; C &gt; &quot;D&quot;", "O escape HTML está incorreto.")) return 1;

    const std::string paragraphs = sto::documents::paragraphs_from_wrapped_text("Linha um\ncontinua\n\nOutro & <texto>", "");
    if (!expect(paragraphs == "<p>Linha um continua</p>\n<p>Outro &amp; &lt;texto&gt;</p>\n", "A conversão de parágrafos está incorreta.")) return 1;

    if (!expect(sto::documents::paragraphs_from_wrapped_text("", "") == "<p></p>\n", "Texto vazio deveria gerar um parágrafo vazio.")) return 1;

    const std::vector<unsigned char> data{'M', 'a', 'n'};
    if (!expect(sto::documents::base64_encode(data) == "TWFu", "A codificação base64 está incorreta.")) return 1;

    sto::documents::DocumentAssets assets;
    assets.logo_base64 = "AA==";
    assets.paged_js = "";

    sto::documents::PagedDocument document;
    document.browser_title = "Teste <HTML>";
    document.document_title = "Documento & Teste";
    document.header_lines = {"Linha <1>"};
    document.footer_html = "    <div>Rodapé</div>\n";
    document.body_html = "  <p>Corpo</p>\n";

    const std::string html = sto::documents::build_paged_html(document, assets);
    if (!expect(html.find("<title>Teste &lt;HTML&gt;</title>") != std::string::npos, "O título do navegador não foi escapado.")) return 1;
    if (!expect(html.find("Documento &amp; Teste") != std::string::npos, "O título do documento não foi escapado.")) return 1;
    if (!expect(html.find("Linha &lt;1&gt;") != std::string::npos, "O cabeçalho não foi escapado.")) return 1;

    return 0;
}
