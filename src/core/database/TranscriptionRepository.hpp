#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace sto::database {
struct TranscriptionPrompts {
    std::string context;
    std::string traditional;
    std::string formal;
};

struct TranscriptionSession {
    long long id = 0;
    std::string input_path;
    std::string party_name;
    std::string additional_context;
    std::string transcript;
    int hearing_type = 0;
    int procedure = 0;
    bool show_context = false;
    std::string status = "draft";
    int queue_order = 0;
    long long group_id = 0;
    std::string date_oitiva;
};

struct TranscriptionGroup {
    long long id = 0;
    std::string name;
    int queue_order = 0;
    bool collapsed = false;
};

class TranscriptionRepository {
public:
    TranscriptionRepository() = default;
    ~TranscriptionRepository();

    TranscriptionRepository(const TranscriptionRepository&) = delete;
    TranscriptionRepository& operator=(const TranscriptionRepository&) = delete;

    bool initialize(const std::filesystem::path& database_path, const TranscriptionPrompts& defaults, std::string& error);
    bool begin_transaction(std::string& error);
    bool commit_transaction(std::string& error);
    bool load_prompts(TranscriptionPrompts& prompts, std::string& error) const;
    bool save_prompts(const TranscriptionPrompts& prompts, std::string& error);
    bool load_sessions(std::vector<TranscriptionSession>& sessions, std::string& error) const;
    bool create_session(TranscriptionSession& session, std::string& error);
    bool save_session(const TranscriptionSession& session, std::string& error);
    bool save_session_order(long long session_id, long long group_id, int queue_order, std::string& error);
    bool delete_session(long long session_id, std::string& error);
    bool load_groups(std::vector<TranscriptionGroup>& groups, std::string& error) const;
    bool create_group(TranscriptionGroup& group, std::string& error);
    bool save_group(const TranscriptionGroup& group, std::string& error);
    bool save_group_order(long long group_id, int queue_order, std::string& error);
    bool delete_group(long long group_id, std::string& error);
    double estimate_seconds(const std::string& processor_model, double media_duration, std::string& error) const;
    bool record_history(const std::string& processor_model, double media_duration, double elapsed_seconds, std::string& error);

private:
    sqlite3* database_ = nullptr;
};
}
