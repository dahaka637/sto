#pragma once

#include <string>

namespace sto::modules::transcription {

struct SessionSnapshot {
    bool has_session = false;
    long long session_id = 0;
    int hearing_type = 0;
    std::string party_name;
    std::string date_oitiva;
};

void render();
void render_prompt_settings();
void render_notification();
SessionSnapshot get_selected_session_snapshot();

}
