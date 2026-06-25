#include "core/database/PromptRepository.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}

int main() {
    const std::filesystem::path test_directory = std::filesystem::temp_directory_path() / "sto-prompt-repository-test";
    std::filesystem::remove_all(test_directory);

    std::string error;
    {
        sto::database::PromptRepository repository;
        if (!expect(repository.initialize(test_directory / "sto.db", error), error.c_str())) return 1;

        sto::database::Prompt prompt{0, "Resumo", "Resuma o texto.", "#445566", "I"};
        if (!expect(repository.save(prompt, error), error.c_str())) return 1;

        auto prompts = repository.list(error);
        if (!expect(prompts.size() == 1, "O prompt não foi persistido.")) return 1;
        if (!expect(prompts[0].title == "Resumo", "O título persistido está incorreto.")) return 1;
        if (!expect(prompts[0].icon == "I", "O ícone persistido está incorreto.")) return 1;

        prompts[0].content = "Resuma o texto em tópicos.";
        prompts[0].icon = "X";
        if (!expect(repository.save(prompts[0], error), error.c_str())) return 1;
        prompts = repository.list(error);
        if (!expect(prompts[0].content == "Resuma o texto em tópicos.", "A atualização não foi persistida.")) return 1;
        if (!expect(prompts[0].icon == "X", "A atualização do ícone não foi persistida.")) return 1;

        if (!expect(repository.remove(prompts[0].id, error), error.c_str())) return 1;
        prompts = repository.list(error);
        if (!expect(prompts.empty(), "A exclusão não foi persistida.")) return 1;
    }

    std::filesystem::remove_all(test_directory);
    return 0;
}
