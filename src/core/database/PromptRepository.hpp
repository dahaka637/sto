#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace sto::database {
struct Prompt {
    int id = 0;
    std::string title;
    std::string content;
    std::string color;
    std::string icon = "\xef\x83\x90";
};

class PromptRepository {
public:
    PromptRepository() = default;
    ~PromptRepository();

    PromptRepository(const PromptRepository&) = delete;
    PromptRepository& operator=(const PromptRepository&) = delete;

    bool initialize(const std::filesystem::path& database_path, std::string& error);
    std::vector<Prompt> list(std::string& error) const;
    bool save(const Prompt& prompt, std::string& error);
    bool remove(int id, std::string& error);

private:
    sqlite3* database_ = nullptr;
};
}
