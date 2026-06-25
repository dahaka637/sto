#pragma once

#include <filesystem>
#include <string>

struct sqlite3;

namespace sto::database {

struct DocumentPreferences {
    std::string nome_delegado;
    std::string nome_responsavel;
    std::string cargo_responsavel;
    std::string local_depoimento_especial;
};

struct DocumentTemplate {
    std::string header_text;    // Cabeçalho: linhas de texto separadas por \n
    std::string footer_text;    // Rodapé: texto simples
    std::string intro_auto;     // Parágrafo introdutório do Auto de Transcrição (com marcadores $$...$$)
    std::string intro_geral;    // Parágrafo introdutório do Auto de Transcrição Geral
    std::string closing_auto;   // Parágrafo de encerramento do Auto de Transcrição
    std::string closing_geral;     // Parágrafo de encerramento do Auto de Transcrição Geral
    std::string oitiva_geral;      // Título de cada oitiva no Auto Geral (com marcadores $$...$$)
    std::string audio_title;       // Título do Auto de Transcrição de Áudios
    std::string audio_intro;       // Parágrafo introdutório do Auto de Áudios
    std::string audio_entry_title; // Título de cada arquivo de áudio
    std::string audio_closing;     // Parágrafo de encerramento do Auto de Áudios
    std::string audio_note_whisper;
    std::string audio_note_model;
    std::string logo_path;         // Vazio = usar logo padrão
};

class DocumentsRepository {
public:
    DocumentsRepository() = default;
    ~DocumentsRepository();

    DocumentsRepository(const DocumentsRepository&) = delete;
    DocumentsRepository& operator=(const DocumentsRepository&) = delete;

    bool initialize(const std::filesystem::path& database_path, std::string& error);
    DocumentPreferences load(std::string& error) const;
    bool save(const DocumentPreferences& preferences, std::string& error);
    DocumentTemplate load_template(std::string& error) const;
    bool save_template(const DocumentTemplate& tmpl, std::string& error);

private:
    sqlite3* database_ = nullptr;
};

}
