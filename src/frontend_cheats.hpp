#pragma once

namespace tooie::frontend::cheats {
void initialize();
void draw_access_gate();
void draw_tools(int category);
void draw_free_camera();
bool revoke_access_from_launcher();
void trigger_graphics_issue_marker() noexcept;
}
