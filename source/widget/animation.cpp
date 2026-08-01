#include <algorithm>
#include <cmath>

#include <im_anim.h>

#include <widget/animation.hpp>

namespace soundstep {

float animation_tween(ImGuiID owner, ImGuiID channel, float target, float duration, int easing)
{
    return iam_tween_float(
        owner,
        channel,
        target,
        duration,
        iam_ease_preset(easing),
        iam_policy_crossfade,
        ImGui::GetIO().DeltaTime);
}

float animation_snap(ImGuiID owner, ImGuiID channel, float target)
{
    return iam_tween_float(
        owner,
        channel,
        target,
        0.0f,
        iam_ease_preset(iam_ease_linear),
        iam_policy_cut,
        ImGui::GetIO().DeltaTime);
}

ImU32 animation_with_alpha(ImVec4 value, float alpha)
{
    value.w *= (std::clamp)(alpha, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(value);
}

bool animation_toggle(const char* label, bool* value, bool compact)
{
    const ImGuiStyle& _style = ImGui::GetStyle();
    const float _height = compact ? 16.0f : 18.0f;
    const float _width = compact ? 29.0f : 34.0f;
    const bool _has_label = label != nullptr && label[0] != '\0'
        && !(label[0] == '#' && label[1] == '#');
    const ImVec2 _text_size = _has_label ? ImGui::CalcTextSize(label) : ImVec2();
    const float _total_width = _width
        + (_has_label ? _style.ItemInnerSpacing.x + _text_size.x : 0.0f);
    const float _total_height = (std::max)(_height, _text_size.y);
    const ImVec2 _position = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(label, ImVec2(_total_width, _total_height));
    const bool _clicked = ImGui::IsItemClicked();
    if (_clicked) {
        *value = !*value;
    }

    const ImGuiID _owner = ImGui::GetItemID();
    const bool _hovered = ImGui::IsItemHovered();
    const float _state = animation_tween(
        _owner,
        0x74001u,
        *value ? 1.0f : 0.0f,
        animation_quick,
        iam_ease_out_cubic);
    const float _hover = animation_tween(
        _owner,
        0x74002u,
        _hovered ? 1.0f : 0.0f,
        animation_quick,
        iam_ease_out_cubic);

    ImVec4 _off = _style.Colors[ImGuiCol_FrameBg];
    ImVec4 _on = _style.Colors[ImGuiCol_HeaderActive];
    ImVec4 _track = iam_get_blended_color(_off, _on, _state, iam_col_oklab);
    _track = iam_get_blended_color(
        _track,
        _style.Colors[ImGuiCol_FrameBgHovered],
        _hover * (*value ? 0.25f : 0.55f),
        iam_col_oklab);

    const float _track_y = _position.y + (_total_height - _height) * 0.5f;
    const ImVec2 _track_min(_position.x, _track_y);
    const ImVec2 _track_max(_position.x + _width, _track_y + _height);
    ImDrawList* _draw = ImGui::GetWindowDrawList();
    _draw->AddRectFilled(
        _track_min,
        _track_max,
        ImGui::GetColorU32(_track),
        _height * 0.5f);

    const float _knob_radius = _height * 0.5f - 2.0f;
    const float _knob_x = _track_min.x + _height * 0.5f
        + (_width - _height) * _state;
    const ImVec4 _knob_color = iam_get_blended_color(
        _style.Colors[ImGuiCol_TextDisabled],
        _style.Colors[ImGuiCol_Text],
        0.65f + _state * 0.35f,
        iam_col_oklab);
    _draw->AddCircleFilled(
        ImVec2(_knob_x, _track_y + _height * 0.5f),
        _knob_radius + _hover * 0.45f,
        ImGui::GetColorU32(_knob_color));

    if (_has_label) {
        _draw->AddText(
            ImVec2(
                _position.x + _width + _style.ItemInnerSpacing.x,
                _position.y + (_total_height - _text_size.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_Text),
            label);
    }
    return _clicked;
}

void animation_activity_indicator(ImGuiID owner, ImVec2 center, float radius, ImU32 color_value)
{
    const float _rotation = iam_oscillate(
        owner,
        IAM_PI,
        0.85f,
        iam_wave_sawtooth,
        0.0f,
        ImGui::GetIO().DeltaTime);
    ImDrawList* _draw = ImGui::GetWindowDrawList();
    _draw->PathArcTo(center, radius, _rotation, _rotation + IAM_PI * 1.35f, 20);
    _draw->PathStroke(color_value, 0, 2.0f);
}

}
