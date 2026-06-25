#include "core/database/PromptRepository.hpp"

#include "sqlite3.h"

namespace sto::database {
namespace {
bool execute(sqlite3* database, const char* sql, std::string& error) {
    char* message = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &message) == SQLITE_OK) {
        return true;
    }

    error = message != nullptr ? message : sqlite3_errmsg(database);
    sqlite3_free(message);
    return false;
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

PromptRepository::~PromptRepository() {
    if (database_ != nullptr) {
        sqlite3_close(database_);
    }
}

bool PromptRepository::initialize(const std::filesystem::path& database_path, std::string& error) {
    std::filesystem::create_directories(database_path.parent_path());
    if (sqlite3_open16(database_path.c_str(), &database_) != SQLITE_OK) {
        error = database_ != nullptr ? sqlite3_errmsg(database_) : "Não foi possível abrir o banco de dados.";
        return false;
    }

    return execute(database_, "PRAGMA journal_mode=WAL;", error)
        && execute(database_, "PRAGMA foreign_keys=ON;", error)
        && execute(database_,
            "CREATE TABLE IF NOT EXISTS prompts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "title TEXT NOT NULL COLLATE NOCASE UNIQUE,"
            "content TEXT NOT NULL,"
            "color TEXT NOT NULL DEFAULT '#FFFFFF',"
            "icon TEXT NOT NULL DEFAULT '\xef\x83\x90',"
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ");",
            error)
        && ensure_column(database_, "prompts", "icon",
            "ALTER TABLE prompts ADD COLUMN icon TEXT NOT NULL DEFAULT '\xef\x83\x90';",
            error);
}

std::vector<Prompt> PromptRepository::list(std::string& error) const {
    std::vector<Prompt> prompts;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql = "SELECT id, title, content, color, icon FROM prompts ORDER BY title COLLATE NOCASE;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return prompts;
    }

    while (sqlite3_step(statement) == SQLITE_ROW) {
        Prompt prompt;
        prompt.id = sqlite3_column_int(statement, 0);
        prompt.title = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        prompt.content = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
        prompt.color = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
        prompt.icon = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
        prompts.push_back(std::move(prompt));
    }
    sqlite3_finalize(statement);
    return prompts;
}

bool PromptRepository::save(const Prompt& prompt, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    const char* sql = prompt.id == 0
        ? "INSERT INTO prompts(title, content, color, icon) VALUES(?1, ?2, ?3, ?4);"
        : "UPDATE prompts SET title=?1, content=?2, color=?3, icon=?4, updated_at=CURRENT_TIMESTAMP WHERE id=?5;";

    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }

    sqlite3_bind_text(statement, 1, prompt.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, prompt.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, prompt.color.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, prompt.icon.c_str(), -1, SQLITE_TRANSIENT);
    if (prompt.id != 0) {
        sqlite3_bind_int(statement, 5, prompt.id);
    }

    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) {
        error = sqlite3_errmsg(database_);
    }
    sqlite3_finalize(statement);
    return success;
}

bool PromptRepository::remove(int id, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, "DELETE FROM prompts WHERE id=?1;", -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_int(statement, 1, id);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    if (!success) {
        error = sqlite3_errmsg(database_);
    }
    sqlite3_finalize(statement);
    return success;
}
}
