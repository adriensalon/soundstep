#include <algorithm>

#include <imgui.h>

#include <core/context.hpp>
#include <widget/app.hpp>
#include <widget/friends.hpp>
#include <widget/library.hpp>
#include <widget/player.hpp>
#include <widget/settings.hpp>

namespace soundstep {

void draw_app(context& ctx)
{
    const ImGuiViewport* _viewport = ImGui::GetMainViewport();
    const ImVec2 _window_padding = ImGui::GetStyle().WindowPadding;
    ImGui::SetNextWindowPos(_viewport->WorkPos);
    ImGui::SetNextWindowSize(_viewport->WorkSize);
    constexpr ImGuiWindowFlags _window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 6.0f));
    if (ImGui::Begin("##SoundstepMain", nullptr, _window_flags)) {
        constexpr float _player_height = 112.0f;
        const float _library_player_gap = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
        const float _content_start_y = ImGui::GetCursorPosY();
        const float _content_height = (std::max)(0.0f, ImGui::GetContentRegionAvail().y - _player_height - _library_player_gap);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, _window_padding);
        if (ImGui::BeginChild("##SoundstepContent", ImVec2(0.0f, _content_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding)) {
            draw_library(ctx);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::SetCursorPosY(_content_start_y + _content_height + _library_player_gap);
        if (ImGui::BeginChild("##SoundstepPlayer", ImVec2(0.0f, 0.0f), false)) {
            draw_player(ctx);
        }
        ImGui::EndChild();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, _window_padding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImGui::GetStyle().FrameRounding);
        draw_friends(ctx);
        draw_settings(ctx);
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
}

}
