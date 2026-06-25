#include "core/database/DocumentsRepository.hpp"

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
    const std::filesystem::path test_directory = std::filesystem::temp_directory_path() / "sto-documents-repository-test";
    std::filesystem::remove_all(test_directory);

    std::string error;
    {
        sto::database::DocumentsRepository repository;
        if (!expect(repository.initialize(test_directory / "sto.db", error), error.c_str())) return 1;

        auto preferences = repository.load(error);
        if (!expect(preferences.nome_delegado.empty(), "As preferências iniciais deveriam estar vazias.")) return 1;

        preferences.nome_delegado = "João da Silva";
        preferences.nome_responsavel = "Maria Aparecida Souza";
        preferences.cargo_responsavel = "Escrivã de Polícia Civil";
        preferences.local_depoimento_especial = "na sala de audiências do Poder Judiciário";
        if (!expect(repository.save(preferences, error), error.c_str())) return 1;

        preferences = repository.load(error);
        if (!expect(preferences.nome_delegado == "João da Silva", "O nome do delegado não foi persistido.")) return 1;
        if (!expect(preferences.nome_responsavel == "Maria Aparecida Souza", "O nome do responsável não foi persistido.")) return 1;
        if (!expect(preferences.cargo_responsavel == "Escrivã de Polícia Civil", "O cargo do responsável não foi persistido.")) return 1;
        if (!expect(preferences.local_depoimento_especial == "na sala de audiências do Poder Judiciário", "O local do depoimento especial não foi persistido.")) return 1;

        preferences.nome_delegado = "Pedro Alves";
        if (!expect(repository.save(preferences, error), error.c_str())) return 1;
        preferences = repository.load(error);
        if (!expect(preferences.nome_delegado == "Pedro Alves", "A atualização do nome do delegado não foi persistida.")) return 1;
        if (!expect(preferences.nome_responsavel == "Maria Aparecida Souza", "O nome do responsável foi perdido na atualização.")) return 1;
        if (!expect(preferences.local_depoimento_especial == "na sala de audiências do Poder Judiciário", "O local do depoimento especial foi perdido na atualização.")) return 1;
    }

    std::filesystem::remove_all(test_directory);
    return 0;
}
