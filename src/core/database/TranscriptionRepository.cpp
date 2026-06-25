#include "core/database/TranscriptionRepository.hpp"

#include "sqlite3.h"

#include <system_error>

namespace sto::database {
namespace {
bool execute(sqlite3* database, const char* sql, std::string& error) {
    char* message = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &message) == SQLITE_OK) return true;
    error = message != nullptr ? message : sqlite3_errmsg(database);
    sqlite3_free(message);
    return false;
}

bool ensure_processor_model_column(sqlite3* database, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA table_info(transcription_history);", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }
    bool found = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char* column = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        if (column != nullptr && std::string(column) == "processor_model") {
            found = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    return found || execute(database,
        "ALTER TABLE transcription_history ADD COLUMN processor_model TEXT NOT NULL DEFAULT 'legacy';",
        error);
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

bool save_prompt(sqlite3* database, const char* key, const std::string& content, bool insert_only, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    const char* sql = insert_only
        ? "INSERT OR IGNORE INTO transcription_prompts(prompt_key, content) VALUES(?1, ?2);"
        : "INSERT INTO transcription_prompts(prompt_key, content) VALUES(?1, ?2) "
          "ON CONFLICT(prompt_key) DO UPDATE SET content=excluded.content, updated_at=CURRENT_TIMESTAMP;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }
    sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 2, content.c_str(), -1, SQLITE_TRANSIENT);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database);
    sqlite3_finalize(statement);
    return success;
}
}

TranscriptionRepository::~TranscriptionRepository() {
    if (database_ != nullptr) sqlite3_close(database_);
}

bool TranscriptionRepository::initialize(const std::filesystem::path& database_path, const TranscriptionPrompts& defaults, std::string& error) {
    std::error_code dir_error;
    std::filesystem::create_directories(database_path.parent_path(), dir_error);
    if (sqlite3_open16(database_path.c_str(), &database_) != SQLITE_OK) {
        error = database_ != nullptr ? sqlite3_errmsg(database_) : "Não foi possível abrir o banco de dados.";
        return false;
    }
    if (!execute(database_, "PRAGMA journal_mode=WAL;", error)
        || !execute(database_, "PRAGMA foreign_keys=ON;", error)
        || !execute(database_,
            "CREATE TABLE IF NOT EXISTS transcription_prompts ("
            "prompt_key TEXT PRIMARY KEY,"
            "content TEXT NOT NULL,"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ");"
            "CREATE TABLE IF NOT EXISTS transcription_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "processor_model TEXT NOT NULL,"
            "media_duration REAL NOT NULL,"
            "elapsed_seconds REAL NOT NULL,"
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ");"
            "CREATE TABLE IF NOT EXISTS transcription_sessions ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "input_path TEXT NOT NULL DEFAULT '',"
            "party_name TEXT NOT NULL DEFAULT '',"
            "additional_context TEXT NOT NULL DEFAULT '',"
            "transcript TEXT NOT NULL DEFAULT '',"
            "hearing_type INTEGER NOT NULL DEFAULT 0,"
            "procedure_type INTEGER NOT NULL DEFAULT 0,"
            "show_context INTEGER NOT NULL DEFAULT 0,"
            "status TEXT NOT NULL DEFAULT 'draft',"
            "queue_order INTEGER NOT NULL DEFAULT 0,"
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ");"
            "CREATE TABLE IF NOT EXISTS transcription_groups ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL DEFAULT '',"
            "queue_order INTEGER NOT NULL DEFAULT 0,"
            "collapsed INTEGER NOT NULL DEFAULT 0,"
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ");",
            error)
        || !ensure_processor_model_column(database_, error)
        || !ensure_column(database_, "transcription_sessions", "queue_order",
            "ALTER TABLE transcription_sessions ADD COLUMN queue_order INTEGER NOT NULL DEFAULT 0;",
            error)
        || !ensure_column(database_, "transcription_sessions", "group_id",
            "ALTER TABLE transcription_sessions ADD COLUMN group_id INTEGER NOT NULL DEFAULT 0;",
            error)
        || !ensure_column(database_, "transcription_sessions", "date_oitiva",
            "ALTER TABLE transcription_sessions ADD COLUMN date_oitiva TEXT NOT NULL DEFAULT '';",
            error)
        || !execute(database_,
            "CREATE INDEX IF NOT EXISTS idx_transcription_history_processor "
            "ON transcription_history(processor_model, id DESC);",
            error)) return false;

    return save_prompt(database_, "context", defaults.context, true, error)
        && save_prompt(database_, "traditional", defaults.traditional, true, error)
        && save_prompt(database_, "formal", defaults.formal, true, error);
}

bool TranscriptionRepository::begin_transaction(std::string& error) {
    return execute(database_, "BEGIN IMMEDIATE;", error);
}

bool TranscriptionRepository::commit_transaction(std::string& error) {
    return execute(database_, "COMMIT;", error);
}

bool TranscriptionRepository::load_prompts(TranscriptionPrompts& prompts, std::string& error) const {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, "SELECT prompt_key, content FROM transcription_prompts;", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const std::string key = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        const std::string content = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        if (key == "context") prompts.context = content;
        else if (key == "traditional") prompts.traditional = content;
        else if (key == "formal") prompts.formal = content;
    }
    sqlite3_finalize(statement);
    return true;
}

bool TranscriptionRepository::save_prompts(const TranscriptionPrompts& prompts, std::string& error) {
    return save_prompt(database_, "context", prompts.context, false, error)
        && save_prompt(database_, "traditional", prompts.traditional, false, error)
        && save_prompt(database_, "formal", prompts.formal, false, error);
}

bool TranscriptionRepository::load_sessions(std::vector<TranscriptionSession>& sessions, std::string& error) const {
    if (!execute(database_, "UPDATE transcription_sessions SET status = 'paused' WHERE status IN ('running', 'queued');", error)) return false;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "SELECT id, input_path, party_name, additional_context, transcript, hearing_type, "
        "procedure_type, show_context, status, queue_order, group_id, date_oitiva "
        "FROM transcription_sessions ORDER BY queue_order ASC, id DESC;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sessions.clear();
    while (sqlite3_step(statement) == SQLITE_ROW) {
        TranscriptionSession session;
        session.id = sqlite3_column_int64(statement, 0);
        session.input_path = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        session.party_name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
        session.additional_context = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
        session.transcript = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
        session.hearing_type = sqlite3_column_int(statement, 5);
        session.procedure = sqlite3_column_int(statement, 6);
        session.show_context = sqlite3_column_int(statement, 7) != 0;
        session.status = reinterpret_cast<const char*>(sqlite3_column_text(statement, 8));
        session.queue_order = sqlite3_column_int(statement, 9);
        session.group_id = sqlite3_column_int64(statement, 10);
        if (const auto* d = sqlite3_column_text(statement, 11)) session.date_oitiva = reinterpret_cast<const char*>(d);
        sessions.push_back(std::move(session));
    }
    sqlite3_finalize(statement);
    return true;
}

bool TranscriptionRepository::create_session(TranscriptionSession& session, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    int next_order = 0;
    if (sqlite3_prepare_v2(database_, "SELECT COALESCE(MIN(queue_order), 0) - 1 FROM transcription_sessions WHERE group_id = 0;", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    if (sqlite3_step(statement) == SQLITE_ROW) next_order = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);

    if (sqlite3_prepare_v2(database_, "INSERT INTO transcription_sessions(queue_order) VALUES(?1);", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_int(statement, 1, next_order);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    if (success) {
        session.id = sqlite3_last_insert_rowid(database_);
        session.queue_order = next_order;
    }
    return success;
}

bool TranscriptionRepository::save_session(const TranscriptionSession& session, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "UPDATE transcription_sessions SET input_path=?1, party_name=?2, additional_context=?3, transcript=?4, "
        "hearing_type=?5, procedure_type=?6, show_context=?7, status=?8, queue_order=?9, group_id=?10, "
        "date_oitiva=?11, updated_at=CURRENT_TIMESTAMP WHERE id=?12;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(statement, 1, session.input_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, session.party_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, session.additional_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, session.transcript.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 5, session.hearing_type);
    sqlite3_bind_int(statement, 6, session.procedure);
    sqlite3_bind_int(statement, 7, session.show_context ? 1 : 0);
    sqlite3_bind_text(statement, 8, session.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 9, session.queue_order);
    sqlite3_bind_int64(statement, 10, session.group_id);
    sqlite3_bind_text(statement, 11, session.date_oitiva.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 12, session.id);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return success;
}

bool TranscriptionRepository::save_session_order(long long session_id, long long group_id, int queue_order, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, "UPDATE transcription_sessions SET queue_order=?1, group_id=?2, updated_at=CURRENT_TIMESTAMP WHERE id=?3;", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_int(statement, 1, queue_order);
    sqlite3_bind_int64(statement, 2, group_id);
    sqlite3_bind_int64(statement, 3, session_id);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return success;
}

bool TranscriptionRepository::delete_session(long long session_id, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, "DELETE FROM transcription_sessions WHERE id=?1;", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_int64(statement, 1, session_id);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return success;
}

bool TranscriptionRepository::load_groups(std::vector<TranscriptionGroup>& groups, std::string& error) const {
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql = "SELECT id, name, queue_order, collapsed FROM transcription_groups ORDER BY queue_order ASC, id ASC;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    groups.clear();
    while (sqlite3_step(statement) == SQLITE_ROW) {
        TranscriptionGroup group;
        group.id = sqlite3_column_int64(statement, 0);
        group.name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        group.queue_order = sqlite3_column_int(statement, 2);
        group.collapsed = sqlite3_column_int(statement, 3) != 0;
        groups.push_back(std::move(group));
    }
    sqlite3_finalize(statement);
    return true;
}

bool TranscriptionRepository::create_group(TranscriptionGroup& group, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    int next_order = 0;
    if (sqlite3_prepare_v2(database_, "SELECT COALESCE(MIN(queue_order), 0) - 1 FROM transcription_groups;", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    if (sqlite3_step(statement) == SQLITE_ROW) next_order = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);

    if (sqlite3_prepare_v2(database_, "INSERT INTO transcription_groups(name, queue_order, collapsed) VALUES(?1, ?2, ?3);", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(statement, 1, group.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, next_order);
    sqlite3_bind_int(statement, 3, group.collapsed ? 1 : 0);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    if (success) {
        group.id = sqlite3_last_insert_rowid(database_);
        group.queue_order = next_order;
    }
    return success;
}

bool TranscriptionRepository::save_group(const TranscriptionGroup& group, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql = "UPDATE transcription_groups SET name=?1, queue_order=?2, collapsed=?3, updated_at=CURRENT_TIMESTAMP WHERE id=?4;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(statement, 1, group.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, group.queue_order);
    sqlite3_bind_int(statement, 3, group.collapsed ? 1 : 0);
    sqlite3_bind_int64(statement, 4, group.id);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return success;
}

bool TranscriptionRepository::save_group_order(long long group_id, int queue_order, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, "UPDATE transcription_groups SET queue_order=?1, updated_at=CURRENT_TIMESTAMP WHERE id=?2;", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_int(statement, 1, queue_order);
    sqlite3_bind_int64(statement, 2, group_id);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return success;
}

bool TranscriptionRepository::delete_group(long long group_id, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, "DELETE FROM transcription_groups WHERE id=?1;", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_int64(statement, 1, group_id);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return success;
}

double TranscriptionRepository::estimate_seconds(const std::string& processor_model, double media_duration, std::string& error) const {
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "SELECT AVG(elapsed_seconds / media_duration) FROM ("
        "SELECT media_duration, elapsed_seconds FROM transcription_history "
        "WHERE processor_model = ?1 AND media_duration > 0 AND elapsed_seconds > 0 "
        "ORDER BY id DESC LIMIT 50"
        ");";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return media_duration * 1.5;
    }
    sqlite3_bind_text(statement, 1, processor_model.c_str(), -1, SQLITE_TRANSIENT);
    double ratio = 1.5;
    if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) != SQLITE_NULL) {
        ratio = sqlite3_column_double(statement, 0);
    }
    sqlite3_finalize(statement);
    return media_duration * ratio;
}

bool TranscriptionRepository::record_history(const std::string& processor_model, double media_duration, double elapsed_seconds, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, "INSERT INTO transcription_history(processor_model, media_duration, elapsed_seconds) VALUES(?1, ?2, ?3);", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(statement, 1, processor_model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(statement, 2, media_duration);
    sqlite3_bind_double(statement, 3, elapsed_seconds);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    if (!success) return false;

    if (sqlite3_prepare_v2(database_,
        "DELETE FROM transcription_history WHERE processor_model = ?1 AND id NOT IN ("
        "SELECT id FROM transcription_history WHERE processor_model = ?1 ORDER BY id DESC LIMIT 50"
        ");",
        -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(statement, 1, processor_model.c_str(), -1, SQLITE_TRANSIENT);
    const bool trimmed = sqlite3_step(statement) == SQLITE_DONE;
    if (!trimmed) error = sqlite3_errmsg(database_);
    sqlite3_finalize(statement);
    return trimmed;
}
}
