// SPDX-License-Identifier: MIT
// Psynder — tiny runtime console host helpers.
//
// This is intentionally narrow: integration layers can call init once (or many
// times) during startup, then pump once per frame to execute queued console
// work on the main thread.

#pragma once

#include "Completion.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace psynder::console {

class Console;

struct RuntimeConsoleInput {
    bool toggle_pressed = false;
    bool escape_pressed = false;
    bool enter_pressed = false;
    bool backspace_pressed = false;
    bool history_prev_pressed = false;
    bool history_next_pressed = false;
    bool backspace_down = false;
    bool history_prev_down = false;
    bool history_next_down = false;
    bool edit_left_pressed = false;
    bool edit_right_pressed = false;
    bool edit_left_down = false;
    bool edit_right_down = false;
    bool edit_home_pressed = false;
    bool edit_end_pressed = false;
    bool edit_delete_pressed = false;
    bool shift_down = false;
    bool copy_pressed = false;
    bool cut_pressed = false;
    bool paste_pressed = false;
    bool select_all_pressed = false;
    bool tab_pressed = false;
    float scroll_delta = 0.0f;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool mouse_left_down = false;
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
    std::string_view paste_text;
    std::string_view text;
};

struct RuntimeConsoleFrame {
    bool open = false;
    bool toggle_consumed = false;
    bool escape_consumed = false;
    bool enter_consumed = false;
    bool backspace_consumed = false;
    bool history_consumed = false;
    bool text_consumed = false;
};

// Register core runtime console cvars and diagnostics commands on the singleton
// console. Safe to call repeatedly; existing cvar values are preserved.
void init_runtime_console();

// Same as above, targeting an explicit console instance for tests / tools.
void init_runtime_console(Console& con);

// Execute queued console work for the singleton console.
void pump_runtime_console();

// Same as above, targeting an explicit console instance for tests / tools.
void pump_runtime_console(Console& con);
void submit_runtime_console_line(std::string_view line);
void submit_runtime_console_line(Console& con, std::string_view line);

// Shared in-game console visibility state. Integration layers feed key edges
// here once per frame; UI/render lanes can query runtime_console_open() when
// drawing the overlay. Escape closes the console before gameplay/window code
// sees it.
RuntimeConsoleFrame update_runtime_console(const RuntimeConsoleInput& input);
RuntimeConsoleFrame update_runtime_console(Console& con,
                                           const RuntimeConsoleInput& input);
bool runtime_console_open();
void set_runtime_console_open(bool open);
bool runtime_console_quit_requested();
bool consume_runtime_console_quit_requested();
bool runtime_console_restart_requested();
bool consume_runtime_console_restart_requested();
std::string_view runtime_console_line();
std::size_t runtime_console_cursor();
std::size_t runtime_console_selection_start();
std::size_t runtime_console_selection_end();
std::span<const std::string> runtime_console_output();
std::size_t runtime_console_output_scroll();
std::span<const CompletionMatch> runtime_console_completions();
std::size_t runtime_console_completion_selected();
void clear_runtime_console_output();

using RuntimeConsoleTextProvider = std::function<std::string()>;
using RuntimeConsoleClipboardSetter = std::function<void(std::string_view)>;

// Optional integration hooks. Samples / engine subsystems install these after
// init_runtime_console(); commands print a clear "unavailable" line until then.
void set_runtime_console_gpu_info_provider(RuntimeConsoleTextProvider provider);
void set_runtime_console_render_stats_provider(RuntimeConsoleTextProvider provider);
void set_runtime_console_scene_stats_provider(RuntimeConsoleTextProvider provider);
void set_runtime_console_script_reload_provider(RuntimeConsoleTextProvider provider);
void set_runtime_console_clipboard_setter(RuntimeConsoleClipboardSetter setter);
void set_runtime_console_engine_launch(int argc, char** argv);
void set_runtime_console_editor_ipc_port(unsigned port);

}  // namespace psynder::console
