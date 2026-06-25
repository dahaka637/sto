#include "core/database/DocumentsRepository.hpp"

#include "sqlite3.h"

namespace sto::database {
namespace {

// Markers available in intro templates ($$DATA_EXTENSO$$, $$NOME_DELEGADO$$, etc.)
// are substituted at document generation time by DocumentsModule.

constexpr const char* kDefaultHeaderText =
    "ESTADO DE SANTA CATARINA\n"
    "POLÍCIA CIVIL\n"
    "DELEGACIA-GERAL\n"
    "DELEGACIA DE POLÍCIA DA COMARCA DE ABELARDO LUZ";

constexpr const char* kDefaultFooterText =
    "<p>Rua S\xC3\xA3o Roque, n\xC2\xBA 1.772, Bairro Aparecida \xE2\x80\x93 CEP: 89.830-000 \xE2\x80\x93 Abelardo Luz/SC</p>\n"
    "<p><b>Telefones:</b> (49) 3445-4142 / 3445-4568 \xE2\x80\x93 <b>E-mail:</b> dpabelardoluz@pc.sc.gov.br \xE2\x80\x93 <b>Site:</b> www.policiacivil.sc.gov.br</p>";

constexpr const char* kDefaultIntroAuto =
    "Aos $$DATA_EXTENSO$$, por determinação do Excelentíssimo Senhor Delegado de Polícia Civil, "
    "$$NOME_DELEGADO$$, procedi à transcrição, DE FORMA RESUMIDA E NÃO NECESSARIAMENTE COM AS MESMAS "
    "PALAVRAS, do teor do(a) $$TIPO_OITIVA$$ de $$NOME_PARTE$$, colhido(a) no dia $$DATA_OITIVA$$, "
    "no âmbito $$PROCEDIMENTO$$, conforme abaixo:";

constexpr const char* kDefaultIntroGeral =
    "Por meio deste documento, aos $$DATA_EXTENSO$$, por determinação do Excelentíssimo Senhor "
    "Delegado de Polícia Civil, $$NOME_DELEGADO$$, procedemos à transcrição, DE FORMA RESUMIDA "
    "E NÃO NECESSARIAMENTE COM AS MESMAS PALAVRAS, do teor das oitivas colhidas no âmbito "
    "$$PROCEDIMENTO$$, na seguinte ordem cronológica:";

constexpr const char* kDefaultClosingAuto =
    "Nada mais havendo a constar, procedo ao encerramento da presente transcrição, "
    "vai devidamente assinado, na forma da Lei.";

constexpr const char* kDefaultClosingGeral =
    "Nada mais havendo a constar, procedo ao encerramento das presentes transcrições, "
    "que vai devidamente assinado, na forma da Lei.";

constexpr const char* kDefaultOitivaGeral =
    "$$NOME_PARTE$$ ($$QUALIFICACAO$$), ouvido(a) no dia $$DATA_OITIVA$$:";

constexpr const char* kDefaultAudioTitle =
    "AUTO DE TRANSCRIÇÃO";

constexpr const char* kDefaultAudioIntro =
    "Aos $$DATA_EXTENSO$$, por determinação do Excelentíssimo Senhor Delegado de Polícia Civil, "
    "$$NOME_DELEGADO$$, procedi à transcrição dos seguintes arquivos de áudio, transcritos "
    "NÃO NECESSARIAMENTE COM AS MESMAS PALAVRAS com o uso da ferramenta Whisper$$NOTA_WHISPER$$, "
    "modelo ggml-large-v3-turbo$$NOTA_MODELO$$, no âmbito $$PROCEDIMENTO$$:";

constexpr const char* kDefaultAudioEntryTitle =
    "$$NUMERAL$$ – $$NOME_ARQUIVO$$$$DESCRICAO_PARENTESES$$:";

constexpr const char* kDefaultAudioClosing =
    "Nada mais havendo a constar, procedo ao encerramento das presentes transcrições de áudio, "
    "que vai devidamente assinado, na forma da Lei.";

constexpr const char* kDefaultAudioNoteWhisper =
    "Whisper é um sistema de reconhecimento automático de fala (ASR) de código aberto desenvolvido pela OpenAI, "
    "capaz de transcrever e traduzir áudio em múltiplos idiomas com alta precisão. Disponível em: "
    "https://github.com/openai/whisper.";

constexpr const char* kDefaultAudioNoteModel =
    "O modelo ggml-large-v3-turbo é uma variante otimizada do modelo Large V3, desenvolvida para oferecer alta "
    "precisão na transcrição multilíngue com desempenho aprimorado em relação às versões anteriores.";

bool execute(sqlite3* database, const char* sql, std::string& error) {
    char* message = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &message) == SQLITE_OK) {
        return true;
    }

    error = message != nullptr ? message : sqlite3_errmsg(database);
    sqlite3_free(message);
    return false;
}


bool insert_text_template_defaults(sqlite3* database, std::string& error) {
    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "INSERT OR IGNORE INTO document_template("
        "id,header_text,footer_text,intro_auto,intro_geral,closing_auto,closing_geral,oitiva_geral,"
        "audio_title,audio_intro,audio_entry_title,audio_closing,audio_note_whisper,audio_note_model,logo_path"
        ") VALUES(1,?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,'');";
    if (sqlite3_prepare_v2(database, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }
    sqlite3_bind_text(stmt, 1, kDefaultHeaderText,   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, kDefaultFooterText,   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, kDefaultIntroAuto,    -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, kDefaultIntroGeral,   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, kDefaultClosingAuto,  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, kDefaultClosingGeral, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, kDefaultOitivaGeral,  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, kDefaultAudioTitle,   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, kDefaultAudioIntro,   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, kDefaultAudioEntryTitle, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, kDefaultAudioClosing, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 12, kDefaultAudioNoteWhisper, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 13, kDefaultAudioNoteModel, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    // Migrate existing rows that have empty fields
    auto fill_if_empty = [&](const char* col, const char* val) {
        const std::string s = std::string("UPDATE document_template SET ") + col + "=?1 WHERE id=1 AND " + col + "='';";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(database, s.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, val, -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    };
    fill_if_empty("header_text",   kDefaultHeaderText);
    fill_if_empty("footer_text",   kDefaultFooterText);
    fill_if_empty("intro_auto",    kDefaultIntroAuto);
    fill_if_empty("intro_geral",   kDefaultIntroGeral);
    fill_if_empty("closing_auto",  kDefaultClosingAuto);
    fill_if_empty("closing_geral", kDefaultClosingGeral);
    fill_if_empty("oitiva_geral",  kDefaultOitivaGeral);
    fill_if_empty("audio_title",   kDefaultAudioTitle);
    fill_if_empty("audio_intro",   kDefaultAudioIntro);
    fill_if_empty("audio_entry_title", kDefaultAudioEntryTitle);
    fill_if_empty("audio_closing", kDefaultAudioClosing);
    fill_if_empty("audio_note_whisper", kDefaultAudioNoteWhisper);
    fill_if_empty("audio_note_model", kDefaultAudioNoteModel);
    return true;
}

bool ensure_column(sqlite3* database, const char* table, const char* column, const char* ddl, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    const std::string pragma = std::string("PRAGMA table_info(") + table + ");";
    if (sqlite3_prepare_v2(database, pragma.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }
    bool found = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char* current = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        if (current != nullptr && std::string(current) == column) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    return found || execute(database, ddl, error);
}
}

DocumentsRepository::~DocumentsRepository() {
    if (database_ != nullptr) {
        sqlite3_close(database_);
    }
}

bool DocumentsRepository::initialize(const std::filesystem::path& database_path, std::string& error) {
    std::filesystem::create_directories(database_path.parent_path());
    if (sqlite3_open16(database_path.c_str(), &database_) != SQLITE_OK) {
        error = database_ != nullptr ? sqlite3_errmsg(database_) : "Não foi possível abrir o banco de dados.";
        return false;
    }

    return execute(database_, "PRAGMA journal_mode=WAL;", error)
        && execute(database_,
            "CREATE TABLE IF NOT EXISTS document_preferences ("
            "id INTEGER PRIMARY KEY CHECK (id = 1),"
            "nome_delegado TEXT NOT NULL DEFAULT '',"
            "nome_responsavel TEXT NOT NULL DEFAULT '',"
            "cargo_responsavel TEXT NOT NULL DEFAULT '',"
            "local_depoimento_especial TEXT NOT NULL DEFAULT ''"
            ");",
            error)
        && ensure_column(database_, "document_preferences", "local_depoimento_especial",
            "ALTER TABLE document_preferences ADD COLUMN local_depoimento_especial TEXT NOT NULL DEFAULT '';",
            error)
        && execute(database_,
            "CREATE TABLE IF NOT EXISTS document_template ("
            "id INTEGER PRIMARY KEY CHECK (id = 1),"
            "header_text TEXT NOT NULL DEFAULT '',"
            "footer_text TEXT NOT NULL DEFAULT '',"
            "intro_auto TEXT NOT NULL DEFAULT '',"
            "intro_geral TEXT NOT NULL DEFAULT '',"
            "closing_auto TEXT NOT NULL DEFAULT '',"
            "closing_geral TEXT NOT NULL DEFAULT '',"
            "audio_title TEXT NOT NULL DEFAULT '',"
            "audio_intro TEXT NOT NULL DEFAULT '',"
            "audio_entry_title TEXT NOT NULL DEFAULT '',"
            "audio_closing TEXT NOT NULL DEFAULT '',"
            "audio_note_whisper TEXT NOT NULL DEFAULT '',"
            "audio_note_model TEXT NOT NULL DEFAULT '',"
            "logo_path TEXT NOT NULL DEFAULT ''"
            ");",
            error)
        && execute(database_,
            "INSERT OR IGNORE INTO document_template(id) VALUES(1);",
            error)
        && ensure_column(database_, "document_template", "header_text",
            "ALTER TABLE document_template ADD COLUMN header_text TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "footer_text",
            "ALTER TABLE document_template ADD COLUMN footer_text TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "intro_auto",
            "ALTER TABLE document_template ADD COLUMN intro_auto TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "intro_geral",
            "ALTER TABLE document_template ADD COLUMN intro_geral TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "closing_auto",
            "ALTER TABLE document_template ADD COLUMN closing_auto TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "closing_geral",
            "ALTER TABLE document_template ADD COLUMN closing_geral TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "oitiva_geral",
            "ALTER TABLE document_template ADD COLUMN oitiva_geral TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "audio_title",
            "ALTER TABLE document_template ADD COLUMN audio_title TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "audio_intro",
            "ALTER TABLE document_template ADD COLUMN audio_intro TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "audio_entry_title",
            "ALTER TABLE document_template ADD COLUMN audio_entry_title TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "audio_closing",
            "ALTER TABLE document_template ADD COLUMN audio_closing TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "audio_note_whisper",
            "ALTER TABLE document_template ADD COLUMN audio_note_whisper TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "audio_note_model",
            "ALTER TABLE document_template ADD COLUMN audio_note_model TEXT NOT NULL DEFAULT '';", error)
        && ensure_column(database_, "document_template", "logo_path",
            "ALTER TABLE document_template ADD COLUMN logo_path TEXT NOT NULL DEFAULT '';", error)
        && insert_text_template_defaults(database_, error);
}

DocumentPreferences DocumentsRepository::load(std::string& error) const {
    DocumentPreferences preferences;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql = "SELECT nome_delegado, nome_responsavel, cargo_responsavel, local_depoimento_especial FROM document_preferences WHERE id = 1;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return preferences;
    }

    if (sqlite3_step(statement) == SQLITE_ROW) {
        preferences.nome_delegado = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        preferences.nome_responsavel = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        preferences.cargo_responsavel = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
        preferences.local_depoimento_especial = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
    }
    sqlite3_finalize(statement);
    return preferences;
}

bool DocumentsRepository::save(const DocumentPreferences& preferences, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "INSERT INTO document_preferences(id, nome_delegado, nome_responsavel, cargo_responsavel, local_depoimento_especial) "
        "VALUES(1, ?1, ?2, ?3, ?4) "
        "ON CONFLICT(id) DO UPDATE SET nome_delegado=?1, nome_responsavel=?2, cargo_responsavel=?3, local_depoimento_especial=?4;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }

    sqlite3_bind_text(statement, 1, preferences.nome_delegado.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, preferences.nome_responsavel.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, preferences.cargo_responsavel.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, preferences.local_depoimento_especial.c_str(), -1, SQLITE_TRANSIENT);

    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) {
        error = sqlite3_errmsg(database_);
    }
    sqlite3_finalize(statement);
    return success;
}

DocumentTemplate DocumentsRepository::load_template(std::string& error) const {
    DocumentTemplate tmpl;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "SELECT header_text,footer_text,intro_auto,intro_geral,closing_auto,closing_geral,oitiva_geral,"
        "audio_title,audio_intro,audio_entry_title,audio_closing,audio_note_whisper,audio_note_model,logo_path "
        "FROM document_template WHERE id=1;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return tmpl;
    }
    if (sqlite3_step(statement) == SQLITE_ROW) {
        auto col = [&](int i) -> std::string {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, i));
            return text ? text : "";
        };
        tmpl.header_text   = col(0);
        tmpl.footer_text   = col(1);
        tmpl.intro_auto    = col(2);
        tmpl.intro_geral   = col(3);
        tmpl.closing_auto  = col(4);
        tmpl.closing_geral = col(5);
        tmpl.oitiva_geral  = col(6);
        tmpl.audio_title   = col(7);
        tmpl.audio_intro   = col(8);
        tmpl.audio_entry_title = col(9);
        tmpl.audio_closing = col(10);
        tmpl.audio_note_whisper = col(11);
        tmpl.audio_note_model = col(12);
        tmpl.logo_path     = col(13);
    }
    sqlite3_finalize(statement);
    return tmpl;
}

bool DocumentsRepository::save_template(const DocumentTemplate& tmpl, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "INSERT INTO document_template(id,header_text,footer_text,intro_auto,intro_geral,"
        "closing_auto,closing_geral,oitiva_geral,audio_title,audio_intro,audio_entry_title,"
        "audio_closing,audio_note_whisper,audio_note_model,logo_path) "
        "VALUES(1,?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) "
        "ON CONFLICT(id) DO UPDATE SET header_text=?1,footer_text=?2,intro_auto=?3,"
        "intro_geral=?4,closing_auto=?5,closing_geral=?6,oitiva_geral=?7,audio_title=?8,"
        "audio_intro=?9,audio_entry_title=?10,audio_closing=?11,audio_note_whisper=?12,"
        "audio_note_model=?13,logo_path=?14;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(statement, 1, tmpl.header_text.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, tmpl.footer_text.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, tmpl.intro_auto.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, tmpl.intro_geral.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, tmpl.closing_auto.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, tmpl.closing_geral.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 7, tmpl.oitiva_geral.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 8, tmpl.audio_title.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 9, tmpl.audio_intro.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 10, tmpl.audio_entry_title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 11, tmpl.audio_closing.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 12, tmpl.audio_note_whisper.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 13, tmpl.audio_note_model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 14, tmpl.logo_path.c_str(),    -1, SQLITE_TRANSIENT);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return success;
}

}
