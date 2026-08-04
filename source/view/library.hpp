#pragma once

namespace soundstep {

struct context;

[[nodiscard]] bool can_play_previous_track(const context& ctx);
[[nodiscard]] bool can_play_next_track(const context& ctx);
void play_previous_track(context& ctx);
void play_next_track(context& ctx);
void draw_library(context& ctx);

}
