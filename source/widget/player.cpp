#include <imgui.h>

#include <core/context.hpp>
#include <widget/player.hpp>

namespace soundstep {
namespace {

    const char* _playback_state_name(playback_state state)
    {
        switch (state) {
        case playback_state::stopped:
            return "Stopped";
        case playback_state::buffering:
            return "Buffering";
        case playback_state::playing:
            return "Playing";
        case playback_state::paused:
            return "Paused";
        case playback_state::finished:
            return "Finished";
        case playback_state::failed:
            return "Failed";
        }
        return "Unknown";
    }

}

void draw_player(context& ctx)
{
    const playback_status _playback = ctx.player.status();
    const float _cover_size = ImGui::GetContentRegionAvail().y;

    if (ctx.current_track) {
        const std::optional<renderer_texture> _cover = ctx.covers.texture(*ctx.current_track);
        if (_cover) {
            ImGui::Image(
                reinterpret_cast<ImTextureID>(_cover->native_id),
                ImVec2(_cover_size, _cover_size));
        } else {
            const ImVec2 _position = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                _position,
                ImVec2(_position.x + _cover_size, _position.y + _cover_size),
                IM_COL32_WHITE);
            ImGui::Dummy(ImVec2(_cover_size, _cover_size));
        }
        ImGui::SameLine(0.0f, 12.0f);
    }

    ImGui::BeginGroup();
    if (ctx.current_track) {
        ImGui::TextUnformatted(
            ctx.current_track->title.empty() ? "Unknown" : ctx.current_track->title.c_str());
        ImGui::TextDisabled(
            "%s",
            ctx.current_track->artist.empty() ? "Unknown" : ctx.current_track->artist.c_str());
    }

    ImGui::Text("Playback: %s", _playback_state_name(_playback.state));
    ImGui::Text(
        "Position %.1fs / %.1fs  (buffered %.1fs)",
        _playback.position_seconds,
        _playback.duration_seconds,
        _playback.buffered_seconds);

    ImGui::BeginDisabled(!_playback.has_source);
    if (ImGui::Button("Play")) {
        ctx.try_action([&ctx] { ctx.player.play(); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause")) {
        ctx.try_action([&ctx] { ctx.player.pause(); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        ctx.try_action([&ctx] { ctx.player.stop(); });
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

}
