#include "core/database/AudioRepository.hpp"

#include "sqlite3.h"

#include <algorithm>

namespace sto::database {
namespace {
bool execute(sqlite3* db, const char* sql, std::string& error) {
    char* msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) == SQLITE_OK) return true;
    error = msg ? msg : sqlite3_errmsg(db);
    sqlite3_free(msg);
    return false;
}

bool ensure_column(sqlite3* db, const char* table, const char* col, const char* ddl, std::string& error) {
    sqlite3_stmt* stmt = nullptr;
    const std::string pragma = std::string("PRAGMA table_info(") + table + ");";
    if (sqlite3_prepare_v2(db, pragma.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db); return false;
    }
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && std::string(name) == col) { found = true; break; }
    }
    sqlite3_finalize(stmt);
    return found || execute(db, ddl, error);
}
}

AudioRepository::~AudioRepository() {
    if (database_) sqlite3_close(database_);
}

bool AudioRepository::initialize(const std::filesystem::path& db_path, std::string& error) {
    std::filesystem::create_directories(db_path.parent_path());
    if (sqlite3_open16(db_path.c_str(), &database_) != SQLITE_OK) {
        error = database_ ? sqlite3_errmsg(database_) : "Não foi possível abrir o banco de dados.";
        return false;
    }
    return execute(database_, "PRAGMA journal_mode=WAL;", error)
        && execute(database_,
            "CREATE TABLE IF NOT EXISTS audio_sessions ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "display_name TEXT NOT NULL DEFAULT '',"
            "description TEXT NOT NULL DEFAULT '',"
            "input_path TEXT NOT NULL DEFAULT '',"
            "transcript TEXT NOT NULL DEFAULT '',"
            "status TEXT NOT NULL DEFAULT 'idle',"
            "queue_order INTEGER NOT NULL DEFAULT 0"
            ");", error)
        && execute(database_,
            "CREATE TABLE IF NOT EXISTS audio_transcription_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "processor_model TEXT NOT NULL DEFAULT '',"
            "media_duration REAL NOT NULL DEFAULT 0,"
            "elapsed_seconds REAL NOT NULL DEFAULT 0,"
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ");", error)
        && execute(database_,
            "CREATE INDEX IF NOT EXISTS idx_audio_history_processor "
            "ON audio_transcription_history(processor_model, id DESC);", error)
        && execute(database_,
            "CREATE TABLE IF NOT EXISTS transcription_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "processor_model TEXT NOT NULL DEFAULT 'legacy',"
            "media_duration REAL NOT NULL DEFAULT 0,"
            "elapsed_seconds REAL NOT NULL DEFAULT 0,"
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ");", error)
        && ensure_column(database_, "audio_sessions", "description",
            "ALTER TABLE audio_sessions ADD COLUMN description TEXT NOT NULL DEFAULT '';", error);
}

bool AudioRepository::create_session(AudioSession& session, std::string& error) {
    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "INSERT INTO audio_sessions(display_name,description,input_path,transcript,status,queue_order) "
        "VALUES(?1,?2,?3,'','idle',(SELECT COALESCE(MAX(queue_order)+1,0) FROM audio_sessions));";
    if (sqlite3_prepare_v2(database_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_); return false;
    }
    sqlite3_bind_text(stmt, 1, session.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session.description.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, session.input_path.c_str(),   -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (ok) {
        session.id = sqlite3_last_insert_rowid(database_);
        sqlite3_stmt* order_stmt = nullptr;
        if (sqlite3_prepare_v2(database_, "SELECT queue_order FROM audio_sessions WHERE id=?1;", -1, &order_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(order_stmt, 1, session.id);
            if (sqlite3_step(order_stmt) == SQLITE_ROW) session.queue_order = sqlite3_column_int(order_stmt, 0);
        }
        if (order_stmt) sqlite3_finalize(order_stmt);
    }
    else error = sqlite3_errmsg(database_);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<AudioSession> AudioRepository::load_sessions(std::string& error) const {
    std::vector<AudioSession> sessions;
    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "SELECT id,display_name,description,input_path,transcript,status,queue_order "
        "FROM audio_sessions ORDER BY queue_order ASC;";
    if (sqlite3_prepare_v2(database_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_); return sessions;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AudioSession s;
        s.id           = sqlite3_column_int64(stmt, 0);
        auto col = [&](int i) -> std::string {
            const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            return t ? t : "";
        };
        s.display_name = col(1);
        s.description  = col(2);
        s.input_path   = col(3);
        s.transcript   = col(4);
        s.status       = col(5);
        s.queue_order  = sqlite3_column_int(stmt, 6);
        sessions.push_back(std::move(s));
    }
    sqlite3_finalize(stmt);
    return sessions;
}

bool AudioRepository::save_session(const AudioSession& session, std::string& error) {
    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "UPDATE audio_sessions SET display_name=?1,description=?2,transcript=?3,status=?4,queue_order=?5 WHERE id=?6;";
    if (sqlite3_prepare_v2(database_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_); return false;
    }
    sqlite3_bind_text(stmt, 1, session.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session.description.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, session.transcript.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, session.status.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,  5, session.queue_order);
    sqlite3_bind_int64(stmt,6, session.id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) error = sqlite3_errmsg(database_);
    sqlite3_finalize(stmt);
    return ok;
}

bool AudioRepository::delete_session(long long id, std::string& error) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database_, "DELETE FROM audio_sessions WHERE id=?1;", -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_); return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) error = sqlite3_errmsg(database_);
    sqlite3_finalize(stmt);
    return ok;
}

double AudioRepository::estimate_seconds(const std::string& processor_model, double media_duration, std::string& error) const {
    if (media_duration <= 0.0) return 0.0;
    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "SELECT media_duration, elapsed_seconds FROM audio_transcription_history "
        "WHERE processor_model=?1 AND media_duration > 0 AND elapsed_seconds > 0 "
        "UNION ALL "
        "SELECT media_duration, elapsed_seconds FROM transcription_history "
        "WHERE processor_model=?1 AND media_duration > 0 AND elapsed_seconds > 0;";
    if (sqlite3_prepare_v2(database_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return 0.0;
    }
    sqlite3_bind_text(stmt, 1, processor_model.c_str(), -1, SQLITE_TRANSIENT);
    double weighted_ratio_sum = 0.0;
    double weight_sum = 0.0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const double historical_duration = sqlite3_column_double(stmt, 0);
        const double elapsed = sqlite3_column_double(stmt, 1);
        if (historical_duration <= 0.0 || elapsed <= 0.0) continue;
        const double similarity = std::min(historical_duration, media_duration) / std::max(historical_duration, media_duration);
        const double weight = 0.25 + similarity;
        weighted_ratio_sum += (elapsed / historical_duration) * weight;
        weight_sum += weight;
    }
    sqlite3_finalize(stmt);
    if (weight_sum <= 0.0) return media_duration * 1.5;
    return std::max(1.0, media_duration * (weighted_ratio_sum / weight_sum));
}

bool AudioRepository::record_history(const std::string& processor_model, double media_duration, double elapsed_seconds, std::string& error) {
    if (media_duration <= 0.0 || elapsed_seconds <= 0.0) return true;
    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "INSERT INTO audio_transcription_history(processor_model, media_duration, elapsed_seconds) VALUES(?1, ?2, ?3);";
    if (sqlite3_prepare_v2(database_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, processor_model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, media_duration);
    sqlite3_bind_double(stmt, 3, elapsed_seconds);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) error = sqlite3_errmsg(database_);
    sqlite3_finalize(stmt);
    if (!ok) return false;

    constexpr const char* prune_sql =
        "DELETE FROM audio_transcription_history WHERE processor_model = ?1 AND id NOT IN ("
        "SELECT id FROM audio_transcription_history WHERE processor_model = ?1 ORDER BY id DESC LIMIT 50"
        ");";
    if (sqlite3_prepare_v2(database_, prune_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, processor_model.c_str(), -1, SQLITE_TRANSIENT);
    const bool pruned = sqlite3_step(stmt) == SQLITE_DONE;
    if (!pruned) error = sqlite3_errmsg(database_);
    sqlite3_finalize(stmt);
    return pruned;
}

}
