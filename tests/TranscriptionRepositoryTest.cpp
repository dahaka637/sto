#include "core/database/TranscriptionRepository.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "sto-transcription-repository-test";
    std::filesystem::remove_all(directory);
    std::string error;
    {
        sto::database::TranscriptionRepository repository;
        if (!expect(repository.initialize(directory / "sto.db", {"context", "traditional", "formal"}, error), error.c_str())) return 1;
        sto::database::TranscriptionPrompts prompts;
        if (!expect(repository.load_prompts(prompts, error), error.c_str())) return 1;
        if (!expect(prompts.context == "context" && prompts.formal == "formal", "Os prompts iniciais não foram persistidos.")) return 1;
        prompts.context = "updated";
        if (!expect(repository.save_prompts(prompts, error), error.c_str())) return 1;
        sto::database::TranscriptionSession session;
        if (!expect(repository.create_session(session, error), error.c_str())) return 1;
        session.input_path = "C:/oitivas/exemplo.mp3";
        session.party_name = "Parte Teste";
        session.transcript = "Texto transcrito";
        session.status = "queued";
        if (!expect(repository.save_session(session, error), error.c_str())) return 1;
        std::vector<sto::database::TranscriptionSession> sessions;
        if (!expect(repository.load_sessions(sessions, error), error.c_str())) return 1;
        if (!expect(sessions.size() == 1 && sessions.front().party_name == "Parte Teste" && sessions.front().status == "paused", "A sessão da fila não foi retomada como pausada.")) return 1;
        if (!expect(repository.save_session_order(session.id, 0, 7, error), error.c_str())) return 1;
        if (!expect(repository.load_sessions(sessions, error) && sessions.front().queue_order == 7, "A ordem manual da fila não foi persistida.")) return 1;

        sto::database::TranscriptionGroup group;
        group.name = "Inquérito Policial 85/2025";
        if (!expect(repository.create_group(group, error), error.c_str())) return 1;
        group.name = "Renomeado";
        group.collapsed = true;
        if (!expect(repository.save_group(group, error), error.c_str())) return 1;
        std::vector<sto::database::TranscriptionGroup> groups;
        if (!expect(repository.load_groups(groups, error), error.c_str())) return 1;
        if (!expect(groups.size() == 1 && groups.front().name == "Renomeado" && groups.front().collapsed, "O separador renomeado/recolhido não foi persistido.")) return 1;
        if (!expect(repository.save_group_order(group.id, 3, error), error.c_str())) return 1;
        if (!expect(repository.load_groups(groups, error) && groups.front().queue_order == 3, "A ordem do separador não foi persistida.")) return 1;
        if (!expect(repository.save_session_order(session.id, group.id, 0, error), error.c_str())) return 1;
        if (!expect(repository.load_sessions(sessions, error) && sessions.front().group_id == group.id, "A sessão não foi associada ao separador.")) return 1;
        if (!expect(repository.save_session_order(session.id, 0, 7, error), error.c_str())) return 1;
        if (!expect(repository.delete_group(group.id, error), error.c_str())) return 1;
        if (!expect(repository.load_groups(groups, error) && groups.empty(), "O separador removido permaneceu na lista.")) return 1;

        if (!expect(repository.delete_session(session.id, error), error.c_str())) return 1;
        if (!expect(repository.load_sessions(sessions, error) && sessions.empty(), "A sessão removida permaneceu no histórico.")) return 1;
        if (!expect(repository.record_history("CPU-A", 100.0, 50.0, error), error.c_str())) return 1;
        if (!expect(std::abs(repository.estimate_seconds("CPU-A", 200.0, error) - 100.0) < 0.01, "A estimativa histórica está incorreta.")) return 1;
        if (!expect(std::abs(repository.estimate_seconds("CPU-B", 200.0, error) - 300.0) < 0.01, "Históricos de processadores diferentes foram misturados.")) return 1;
        for (int index = 0; index < 50; ++index) {
            if (!expect(repository.record_history("CPU-A", 100.0, 100.0, error), error.c_str())) return 1;
        }
        if (!expect(std::abs(repository.estimate_seconds("CPU-A", 200.0, error) - 200.0) < 0.01, "O limite dos 50 registros mais recentes não foi aplicado.")) return 1;
        if (!expect(repository.record_history("CPU-B", 100.0, 200.0, error), error.c_str())) return 1;
        if (!expect(std::abs(repository.estimate_seconds("CPU-B", 200.0, error) - 400.0) < 0.01, "A estimativa por processador está incorreta.")) return 1;
    }
    std::filesystem::remove_all(directory);
    return 0;
}
