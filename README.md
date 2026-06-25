# STO

**Sistema de Transcrição de Oitivas e Áudios**

O STO é um aplicativo desktop para Windows voltado ao apoio de rotinas documentais em unidade policial. A proposta é centralizar, em uma interface única e local, ferramentas para transcrição automática por IA, organização de prompts, geração de autos, tratamento de arquivos e apoio operacional.

O projeto é desenvolvido em C++ com Dear ImGui e DirectX 11, priorizando execução local, desempenho, portabilidade dentro do ambiente Windows e uma experiência visual profissional.

## Status dos módulos

| Módulo | Situação | Resumo |
| --- | --- | --- |
| Início | Funcional | Área de apresentação do sistema, cartões dos módulos ativos e informações gerais. |
| Transcrição de Oitivas | Funcional | Fila de transcrição local com Whisper, contexto adicional, prompts editáveis, estimativa de tempo, histórico em SQLite e geração de autos. |
| Transcrição de Áudios | Funcional | Transcrição em lote de arquivos de áudio, seleção múltipla, pausa/retomada, estimativa de tempo, barra de progresso e Auto de Transcrição de Áudios configurável. |
| Prompts para IA | Funcional | Biblioteca de prompts reutilizáveis com título, conteúdo, cor, ícone e composição para cópia. |
| Ferramentas Diversas | Funcional | Conversão de áudio, compressão, divisão, separação e junção de PDF, conversão de PDF para Word, correção de codec de vídeo e correção de nomes. |
| Configurações | Funcional | Preferências visuais e edição dos textos usados nos documentos gerados. |
| Coleta de Provas Digitais | Planejado | Interface reservada para futura gestão de casos, custódia e relatórios. |

## Tecnologias

- C++23
- CMake 3.28+
- Dear ImGui
- DirectX 11
- Win32 API
- SQLite
- Font Awesome
- FFmpeg, Ghostscript, Whisper e pdf2docx como runtimes externos opcionais

## Estrutura

```text
src/
  core/             Infraestrutura compartilhada: banco, arquivos, processos, textos e diálogos.
  modules/          Módulos visuais e funcionais do STO.
  platform/windows/ Inicialização Win32, DirectX 11, janela principal e assets.
  resources/        Ícone do executável, logo e recursos nativos.
  ui/               Tema, menu principal e componentes globais.
tests/              Testes automatizados de repositórios e execução de processos.
tools/              Utilitários auxiliares usados por runtimes externos.
```

## Pré-requisitos

- Windows 10 ou superior
- Visual Studio 2022 com workload de C++
- CMake 3.28 ou superior
- Conexão com a internet na primeira configuração, para baixar dependências via `FetchContent`

## Compilação rápida

```powershell
cmake --preset vs2022-x64
cmake --build --preset release
```

O executável será gerado em:

```text
build/vs2022-x64/Release/STO.exe
```

Também é possível usar o fluxo manual:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Runtimes opcionais

O projeto compila sem os runtimes externos, mas alguns módulos precisam deles em tempo de execução. Esses arquivos são pesados e devem ficar fora do Git.

Para empacotar FFmpeg e Ghostscript:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DSTO_RUNTIME_SOURCE="C:/caminho/para/patch_fix_toolkit"
```

Para empacotar o conversor PDF para Word:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DSTO_PDF2DOCX_SOURCE="C:/caminho/para/pdf2docx"
```

Para empacotar Whisper, modelo e prompts iniciais:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DSTO_TRANSCRIPTION_SOURCE="C:/caminho/para/transcritor de oitivas"
```

## Testes

```powershell
ctest --preset release --output-on-failure
```

No fluxo manual:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

## Dados locais

O STO cria seus dados ao lado do executável, dentro de `data/`, incluindo o banco `sto.db` e arquivos auxiliares. Esses arquivos não devem ser versionados, pois representam dados locais do usuário ou da unidade.

## Observações para GitHub

- `build/`, bancos SQLite, dados locais, crash reports, caches, runtimes copiados e arquivos temporários ficam ignorados pelo Git.
- `src/runtime/` é uma pasta local pesada para runtimes e modelos; ela não deve ser enviada ao repositório.
- Ícone, logo e recursos oficiais estão em `src/resources/`.
- Dependências de terceiros são baixadas pelo CMake durante a configuração.
- Runtimes como modelos Whisper, executáveis FFmpeg/Ghostscript/pdf2docx e DLLs auxiliares devem ser distribuídos por fora ou copiados localmente no ambiente de release.
