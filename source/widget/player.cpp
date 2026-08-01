#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>

#include <im_anim.h>
#include <imgui.h>

#include <core/context.hpp>
#include <widget/animation.hpp>
#include <widget/icons.hpp>
#include <widget/library.hpp>
#include <widget/player.hpp>

namespace soundstep {
namespace {

    std::string _time_text(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0.0) {
            return "--:--";
        }
        const unsigned long long _total_seconds = static_cast<unsigned long long>(seconds);
        const unsigned long long _minutes = _total_seconds / 60;
        const unsigned long long _remaining_seconds = _total_seconds % 60;
        char _value[32] { };
        std::snprintf(_value, sizeof(_value), "%llu:%02llu", _minutes, _remaining_seconds);
        return _value;
    }

    std::string _display_artist(std::string_view value)
    {
        std::string _result;
        std::string_view::size_type _start = 0;
        while (_start <= value.size()) {
            const std::string_view::size_type _separator = value.find('/', _start);
            std::string_view _artist = value.substr(_start, _separator - _start);
            while (!_artist.empty() && std::isspace(static_cast<unsigned char>(_artist.front()))) {
                _artist.remove_prefix(1);
            }
            while (!_artist.empty() && std::isspace(static_cast<unsigned char>(_artist.back()))) {
                _artist.remove_suffix(1);
            }
            if (!_artist.empty()) {
                if (!_result.empty()) {
                    _result += ", ";
                }
                _result.append(_artist.data(), _artist.size());
            }
            if (_separator == std::string_view::npos) {
                break;
            }
            _start = _separator + 1;
        }
        return _result;
    }

    const char* _primary_action_label(playback_state state)
    {
        switch (state) {
        case playback_state::playing:
        case playback_state::buffering:
            return icons::pause_action;
        case playback_state::paused:
            return icons::resume_action;
        case playback_state::finished:
            return icons::replay_action;
        case playback_state::failed:
            return icons::retry_action;
        case playback_state::stopped:
            return icons::play_action;
        }
        return icons::play_action;
    }

    void _draw_extension_badge(audio_extension extension)
    {
        std::string _label(audio_extension_name(extension));
        if (_label.empty()) {
            _label = "unknown";
        }

        constexpr ImVec2 _padding(7.0f, 1.0f);
        const ImVec2 _text_size = ImGui::CalcTextSize(_label.c_str());
        const ImVec2 _size(
            _text_size.x + _padding.x * 2.0f,
            _text_size.y + _padding.y * 2.0f);
        const ImVec2 _position = ImGui::GetCursorScreenPos();
        ImDrawList* _draw = ImGui::GetWindowDrawList();
        _draw->AddRectFilled(
            _position,
            ImVec2(_position.x + _size.x, _position.y + _size.y),
            ImGui::GetColorU32(ImGuiCol_HeaderActive),
            ImGui::GetStyle().FrameRounding);
        _draw->AddText(
            ImVec2(_position.x + _padding.x, _position.y + _padding.y),
            ImGui::GetColorU32(ImGuiCol_Text),
            _label.c_str());
        ImGui::Dummy(_size);
    }

    void _draw_seek_bar(
        context& ctx,
        const playback_status& playback,
        double duration_seconds,
        float width,
        float height)
    {
        static bool _seeking = false;
        static bool _played_position_initialized = false;
        static double _preview_seconds = 0.0;
        static double _last_display_seconds = 0.0;
        static double _last_duration_seconds = 0.0;

        const bool _can_seek = playback.has_source && duration_seconds > 0.0
            && std::isfinite(duration_seconds);
        const ImVec2 _position = ImGui::GetCursorScreenPos();
        const ImGuiID _motion_owner = ImGui::GetID("##PlaybackSeekMotion");
        ImGui::InvisibleButton("##PlaybackSeek", ImVec2(width, height));

        if (_can_seek && ImGui::IsItemActive()) {
            const float _ratio = (std::clamp)((ImGui::GetIO().MousePos.x - _position.x) / width,
                0.0f,
                1.0f);
            _preview_seconds = static_cast<double>(_ratio) * duration_seconds;
            _seeking = true;
        }
        bool _seek_committed = false;
        if (_seeking && ImGui::IsItemDeactivated()) {
            const double _target = _preview_seconds;
            _seek_committed = ctx.try_action([&ctx, _target] { ctx.player.seek(_target); });
            _seeking = false;
        }
        if (!_can_seek) {
            _seeking = false;
        }

        const double _display_seconds = _seeking || _seek_committed
            ? _preview_seconds
            : playback.position_seconds;
        const float _played_target = _can_seek
            ? static_cast<float>((std::clamp)(_display_seconds / duration_seconds, 0.0, 1.0))
            : 0.0f;
        const float _buffered_target = _can_seek
            ? static_cast<float>((std::clamp)((playback.position_seconds + playback.buffered_seconds) / duration_seconds,
                  0.0,
                  1.0))
            : 0.0f;
        const double _maximum_continuous_step = (std::max)(0.25,
            static_cast<double>(ImGui::GetIO().DeltaTime) * 4.0);
        const bool _duration_changed = _played_position_initialized
            && std::abs(duration_seconds - _last_duration_seconds) > 0.01;
        const bool _position_jumped = _played_position_initialized
            && std::abs(_display_seconds - _last_display_seconds) > _maximum_continuous_step;
        const bool _snap_played_position = !_can_seek
            || !_played_position_initialized
            || _seeking
            || _seek_committed
            || _duration_changed
            || _position_jumped;
        const float _played_ratio = _snap_played_position
            ? animation_snap(_motion_owner, 0x21001u, _played_target)
            : animation_tween(
                  _motion_owner,
                  0x21001u,
                  _played_target,
                  0.12f,
                  iam_ease_out_cubic);
        if (_can_seek) {
            _played_position_initialized = true;
            _last_display_seconds = _display_seconds;
            _last_duration_seconds = duration_seconds;
        } else {
            _played_position_initialized = false;
        }
        const float _buffered_ratio = animation_tween(
            _motion_owner,
            0x21002u,
            _buffered_target,
            animation_relaxed,
            iam_ease_out_cubic);
        const bool _hovered = ImGui::IsItemHovered();
        const bool _active = ImGui::IsItemActive();
        const float _thumb_reveal = animation_tween(
            _motion_owner,
            0x21003u,
            (_hovered || _active) && _can_seek ? 1.0f : 0.0f,
            animation_quick,
            iam_ease_out_cubic);

        ImDrawList* _draw = ImGui::GetWindowDrawList();
        const ImVec2 _end(_position.x + width, _position.y + height);
        const float _rounding = height * 0.5f;
        _draw->AddRectFilled(_position, _end, ImGui::GetColorU32(ImGuiCol_FrameBg), _rounding);
        if (_buffered_ratio > 0.0f) {
            _draw->AddRectFilled(
                _position,
                ImVec2(_position.x + width * _buffered_ratio, _end.y),
                ImGui::GetColorU32(ImGuiCol_ButtonHovered),
                _rounding);
        }
        if (_played_ratio > 0.0f) {
            _draw->AddRectFilled(
                _position,
                ImVec2(_position.x + width * _played_ratio, _end.y),
                ImGui::GetColorU32(ImGuiCol_CheckMark),
                _rounding);
        }
        if (_thumb_reveal > 0.001f) {
            const ImVec2 _thumb_position(
                _position.x + width * _played_ratio,
                _position.y + height * 0.5f);
            _draw->AddCircleFilled(
                _thumb_position,
                height * (0.8f + 0.38f * _thumb_reveal),
                animation_with_alpha(
                    ImGui::GetStyleColorVec4(ImGuiCol_CheckMark),
                    0.14f * _thumb_reveal));
            _draw->AddCircleFilled(
                _thumb_position,
                height * (0.42f + 0.38f * _thumb_reveal),
                animation_with_alpha(
                    ImGui::GetStyleColorVec4(ImGuiCol_CheckMark),
                    _thumb_reveal));
        }
    }

    bool _draw_transport_button(
        const char* label,
        ImVec2 size,
        bool enabled,
        ImGuiID channel)
    {
        const ImGuiID _owner = ImGui::GetID(label);
        const ImVec2 _position = ImGui::GetCursorScreenPos();
        const bool _hovered = enabled && ImGui::IsMouseHoveringRect(_position, ImVec2(_position.x + size.x, _position.y + size.y));
        const float _hover = animation_tween(
            _owner,
            channel,
            _hovered ? 1.0f : 0.0f,
            animation_quick,
            iam_ease_out_cubic);
        const ImVec4 _button_color = iam_get_blended_color(
            ImGui::GetStyleColorVec4(ImGuiCol_Button),
            ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered),
            _hover,
            iam_col_oklab);
        ImGui::BeginDisabled(!enabled);
        ImGui::PushStyleColor(ImGuiCol_Button, _button_color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, _button_color);
        const bool _clicked = ImGui::Button(label, size);
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        return _clicked;
    }

    bool _draw_primary_button(
        const char* label,
        ImVec2 size,
        bool enabled,
        bool active,
        bool buffering)
    {
        static std::string _displayed_label;
        static std::string _outgoing_label;
        static int _transition_serial = 0;
        if (_displayed_label.empty()) {
            _displayed_label = label;
        } else if (_displayed_label != label) {
            _outgoing_label = _displayed_label;
            _displayed_label = label;
            ++_transition_serial;
        }

        const ImGuiID _owner = ImGui::GetID("##PrimaryTransportMotion");
        const ImVec2 _position = ImGui::GetCursorScreenPos();
        const bool _hovered = enabled && ImGui::IsMouseHoveringRect(_position, ImVec2(_position.x + size.x, _position.y + size.y));
        const float _hover = animation_tween(
            _owner,
            0x22001u,
            _hovered ? 1.0f : 0.0f,
            animation_quick,
            iam_ease_out_cubic);
        const float _active = animation_tween(
            _owner,
            0x22002u,
            active ? 1.0f : 0.0f,
            animation_normal,
            iam_ease_out_cubic);
        const float _pulse = buffering
            ? iam_oscillate(
                  _owner,
                  0.055f,
                  0.75f,
                  iam_wave_sine,
                  0.0f,
                  ImGui::GetIO().DeltaTime)
            : 0.0f;

        const ImVec4 _idle_color = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
        const ImVec4 _playing_color(0.48f + _pulse, 0.36f + _pulse * 0.65f, 0.16f, 1.0f);
        ImVec4 _background = iam_get_blended_color(
            _idle_color,
            _playing_color,
            _active,
            iam_col_oklab);
        const ImVec4 _hover_color = active
            ? ImVec4(0.58f, 0.44f, 0.20f, 1.0f)
            : ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);
        _background = iam_get_blended_color(_background, _hover_color, _hover, iam_col_oklab);

        ImGui::BeginDisabled(!enabled);
        const bool _clicked = ImGui::InvisibleButton("##PrimaryTransport", size);
        ImGui::EndDisabled();
        ImDrawList* _draw = ImGui::GetWindowDrawList();
        _draw->AddRectFilled(
            _position,
            ImVec2(_position.x + size.x, _position.y + size.y),
            animation_with_alpha(_background, enabled ? 1.0f : ImGui::GetStyle().DisabledAlpha),
            ImGui::GetStyle().FrameRounding);

        ImGui::PushID(_transition_serial);
        const ImGuiID _transition_owner = ImGui::GetID("##PrimaryLabelTransition");
        const float _incoming = animation_tween(
            _transition_owner,
            0x22003u,
            1.0f,
            animation_normal,
            iam_ease_out_cubic);
        ImGui::PopID();
        const auto _draw_label = [&](const std::string& text, float alpha, float y_offset) {
            if (text.empty() || alpha <= 0.001f) {
                return;
            }
            const ImVec2 _text_size = ImGui::CalcTextSize(text.c_str());
            _draw->AddText(
                ImVec2(
                    _position.x + (size.x - _text_size.x) * 0.5f,
                    _position.y + (size.y - _text_size.y) * 0.5f + y_offset),
                animation_with_alpha(
                    ImGui::GetStyleColorVec4(ImGuiCol_Text),
                    alpha * (enabled ? 1.0f : ImGui::GetStyle().DisabledAlpha)),
                text.c_str());
        };
        _draw_label(_outgoing_label, 1.0f - _incoming, -3.0f * _incoming);
        _draw_label(_displayed_label, _incoming, 3.0f * (1.0f - _incoming));
        if (_incoming >= 0.999f) {
            _outgoing_label.clear();
        }
        return _clicked;
    }

}

void draw_player(context& ctx)
{
    constexpr float _player_edge_padding = 28.0f;
    const playback_status _playback = ctx.player.status();
    const float _cover_size = (std::max)(0.0f,
        ImGui::GetContentRegionAvail().y - _player_edge_padding);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + _player_edge_padding);
    if (ctx.current_track) {
        static std::string _last_cover_identity;
        static int _cover_transition_serial = 0;
        const std::string _cover_identity = ctx.current_track->catalog_id + ":" + ctx.current_track->id;
        if (_last_cover_identity != _cover_identity) {
            _last_cover_identity = _cover_identity;
            ++_cover_transition_serial;
        }
        ImGui::PushID(_cover_transition_serial);
        const ImGuiID _cover_motion_owner = ImGui::GetID("##PlayerCoverMotion");
        const float _cover_reveal = animation_tween(
            _cover_motion_owner,
            0x23001u,
            1.0f,
            animation_relaxed,
            iam_ease_out_cubic);
        ImGui::PopID();
        const std::optional<renderer_texture> _cover = ctx.covers.texture(*ctx.current_track);
        const ImVec2 _position = ImGui::GetCursorScreenPos();
        const ImVec2 _cover_end(_position.x + _cover_size, _position.y + _cover_size);
        const float _cover_scale = 0.92f + 0.08f * _cover_reveal;
        const float _cover_inset = _cover_size * (1.0f - _cover_scale) * 0.5f;
        const ImVec2 _animated_cover_min(_position.x + _cover_inset, _position.y + _cover_inset);
        const ImVec2 _animated_cover_max(_cover_end.x - _cover_inset, _cover_end.y - _cover_inset);
        const float _rounding = ImGui::GetStyle().FrameRounding;
        if (_cover) {
            ImGui::GetWindowDrawList()->AddImageRounded(
                reinterpret_cast<ImTextureID>(_cover->native_id),
                _animated_cover_min,
                _animated_cover_max,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f),
                animation_with_alpha(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), _cover_reveal),
                _rounding);
        } else {
            ImGui::GetWindowDrawList()->AddRectFilled(
                _animated_cover_min,
                _animated_cover_max,
                animation_with_alpha(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), _cover_reveal),
                _rounding);
        }
        ImGui::Dummy(ImVec2(_cover_size, _cover_size));
        ImGui::SameLine(0.0f, 12.0f);
    }
    ImGui::BeginGroup();
    const ImGuiStyle& _style = ImGui::GetStyle();
    const ImVec2 _panel_start = ImGui::GetCursorPos();
    const ImVec2 _panel_screen_start = ImGui::GetCursorScreenPos();
    const float _panel_width = (std::max)(0.0f, ImGui::GetContentRegionAvail().x - _player_edge_padding);
    const bool _is_active = _playback.state == playback_state::playing || _playback.state == playback_state::buffering;
    const char* _primary_label = _primary_action_label(_playback.state);
    float _button_width = (std::max)(0.f, ImGui::CalcTextSize(icons::resume_action).x);
    _button_width += _style.FramePadding.x * 2.0f;
    const float _control_height = ImGui::GetFrameHeight();
    constexpr float _transport_spacing = 6.0f;
    const float _controls_width = _button_width * 3.0f + _transport_spacing * 2.0f;
    const float _controls_x = (std::max)(_panel_start.x, _panel_start.x + _panel_width - _controls_width);

    if (ctx.current_track) {
        ImGui::PushFont(ctx.fonts.track_title);
        ImGui::TextUnformatted(ctx.current_track->title.empty() ? "Unknown" : ctx.current_track->title.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        _draw_extension_badge(ctx.current_track->extension);

        std::string _subtitle = _display_artist(ctx.current_track->artist.empty() ? "Unknown" : ctx.current_track->artist);
        if (!ctx.current_track->album.empty()) {
            _subtitle += "  -  " + ctx.current_track->album;
        }
        if (ctx.current_track->track_number != 0) {
            _subtitle += "  #" + std::to_string(ctx.current_track->track_number);
        }
        constexpr float _card_title_subtitle_offset = 24.0f;
        ImGui::SetCursorPosY(_panel_start.y + _card_title_subtitle_offset);
        ImGui::PushClipRect(
            _panel_screen_start,
            ImVec2(
                _panel_screen_start.x + (_controls_x - _panel_start.x) - _transport_spacing,
                _panel_screen_start.y + _cover_size),
            true);
        ImGui::PushFont(ctx.fonts.subtitle);
        ImGui::TextDisabled("%s", _subtitle.c_str());
        ImGui::PopFont();
        ImGui::PopClipRect();
    }

    const double _duration_seconds = ctx.current_track && ctx.current_track->duration_ms != 0
        ? static_cast<double>(ctx.current_track->duration_ms) / 1000.0
        : _playback.duration_seconds;
    const double _elapsed_seconds = _duration_seconds > 0.0
        ? (std::clamp)(_playback.position_seconds, 0.0, _duration_seconds)
        : _playback.position_seconds;
    const std::string _elapsed = _time_text(_elapsed_seconds);
    const std::string _duration = _duration_seconds > 0.0
        ? _time_text(_duration_seconds)
        : "--:--";
    const float _elapsed_width = ImGui::CalcTextSize(_elapsed.c_str()).x;
    const float _duration_width = ImGui::CalcTextSize(_duration.c_str()).x;
    constexpr float _minimum_seek_width = 80.0f;
    const bool _show_times = _panel_width
        >= _elapsed_width + _duration_width + _minimum_seek_width + _style.ItemSpacing.x * 2.0f;
    const float _seek_width = (std::max)(_minimum_seek_width,
        _panel_width - (_show_times ? _elapsed_width + _duration_width + _style.ItemSpacing.x * 2.0f : 0.0f));

    const float _progress_row_y = _panel_start.y + _cover_size - ImGui::GetTextLineHeight();
    const float _controls_y = _panel_start.y;
    ImGui::SetCursorPos(ImVec2(_controls_x, _controls_y));

    ImGui::PushStyleVar(
        ImGuiStyleVar_ItemSpacing,
        ImVec2(_transport_spacing, _style.ItemSpacing.y));
    const bool _can_previous = can_play_previous_track(ctx);
    if (_draw_transport_button(
            icons::previous,
            ImVec2(_button_width, _control_height),
            _can_previous,
            0x24001u)) {
        play_previous_track(ctx);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Previous");
    }
    ImGui::SameLine();
    if (_draw_primary_button(
            _primary_label,
            ImVec2(_button_width, _control_height),
            _playback.has_source,
            _is_active,
            _playback.state == playback_state::buffering)) {
        if (_is_active) {
            ctx.try_action([&ctx] { ctx.player.pause(); });
        } else {
            ctx.try_action([&ctx] { ctx.player.play(); });
        }
    }
    ImGui::SameLine();
    const bool _can_next = can_play_next_track(ctx);
    if (_draw_transport_button(
            icons::next,
            ImVec2(_button_width, _control_height),
            _can_next,
            0x24002u)) {
        play_next_track(ctx);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Next");
    }
    ImGui::PopStyleVar();

    if (_show_times) {
        ImGui::SetCursorPos(ImVec2(_panel_start.x, _progress_row_y));
        ImGui::TextDisabled("%s", _elapsed.c_str());
    }
    constexpr float _seek_height = 8.0f;
    const float _seek_x = _panel_start.x
        + (_show_times ? _elapsed_width + _style.ItemSpacing.x : 0.0f);
    const float _seek_y = _progress_row_y
        + (ImGui::GetTextLineHeight() - _seek_height) * 0.5f
        + 2.0f;
    ImGui::SetCursorPos(ImVec2(_seek_x, _seek_y));
    _draw_seek_bar(ctx, _playback, _duration_seconds, _seek_width, _seek_height);
    if (_show_times) {
        ImGui::SetCursorPos(ImVec2(
            _panel_start.x + _panel_width - _duration_width,
            _progress_row_y));
        ImGui::TextDisabled("%s", _duration.c_str());
    }
    ImGui::SetCursorPos(ImVec2(_panel_start.x, _panel_start.y + _cover_size));
    ImGui::Dummy(ImVec2(_panel_width, 0.0f));
    ImGui::EndGroup();
}

}
