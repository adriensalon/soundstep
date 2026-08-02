#pragma once

#include <imgui.h>

namespace soundstep {

inline void draw_tooltip(const char* text)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    if (ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

}
