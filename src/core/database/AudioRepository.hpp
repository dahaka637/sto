#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace sto::database {

struct AudioSession {
    long long id = 0;
    std::string display_name; // editable label (filename by default)
    std::string description;  // optional user description shown beside filename in document
    std::string input_path;
    std::string transcript;
    std::string status;       // idle | queued | running | paused | completed | failed
    int queue_order = 0;
};

class AudioRepository {
public:
    AudioRepository() = default;
    ~AudioRepository();

    AudioRepository(const AudioRepository&) = delete;
    AudioRepository& operator=(const AudioRepository&) = delete;

    bool initialize(const std::filesystem::path& database_path, std::string& error);
    bool create_session(AudioSession& session, std::string& error);
    std::vector<AudioSession> load_sessions(std::string& error) const;
    bool save_session(const AudioSession& session, std::string& error);
    bool delete_session(long long id, std::string& error);
    double estimate_seconds(const std::string& processor_model, double media_duration, std::string& error) const;
    bool record_history(const std::string& processor_model, double media_duration, double elapsed_seconds, std::string& error);

private:
    sqlite3* database_ = nullptr;
};

}
