#pragma once

#include <imgui.h>

namespace soundstep {

inline constexpr float animation_quick = 0.14f;
inline constexpr float animation_normal = 0.22f;
inline constexpr float animation_relaxed = 0.32f;

[[nodiscard]] float animation_tween(ImGuiID owner, ImGuiID channel, float target, float duration = animation_normal, int easing = 5);
[[nodiscard]] float animation_snap(ImGuiID owner, ImGuiID channel, float target);
[[nodiscard]] ImU32 animation_with_alpha(ImVec4 color, float alpha);
[[nodiscard]] bool animation_toggle(const char* label, bool* value, bool compact = false);
void animation_activity_indicator(ImGuiID owner, ImVec2 center, float radius, ImU32 color);

}
