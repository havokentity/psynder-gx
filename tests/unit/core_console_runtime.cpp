// SPDX-License-Identifier: MIT
// Unit tests for the tiny runtime console host/pump helpers.

#include "core/console/Console.h"
#include "core/console/RuntimeConsole.h"

#include <catch2/catch_test_macros.hpp>

namespace cn = psynder::console;

namespace {

cn::RuntimeConsoleInput input(bool toggle,
                              bool escape,
                              bool enter = false,
                              bool backspace = false,
                              std::string_view text = {},
                              bool history_prev = false,
                              bool history_next = false,
                              bool tab = false,
                              float scroll = 0.0f) {
    return cn::RuntimeConsoleInput{
        toggle,
        escape,
        enter,
        backspace,
        history_prev,
        history_next,
        backspace,
        history_prev,
        history_next,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        tab,
        scroll,
        0.0f,
        0.0f,
        false,
        1280.0f,
        720.0f,
        {},
        text};
}

}  // namespace

TEST_CASE("runtime console init registers core cvars and diag commands",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();

    cn::init_runtime_console(C);
    auto* smart = C.FindCVar("r_console_smart_resolve");
    REQUIRE(smart != nullptr);
    REQUIRE(smart->default_value == "1");
    REQUIRE((smart->flags & cn::CVAR_ARCHIVE) != 0);
    REQUIRE(C.FindCVar("editor_web_root") != nullptr);
    REQUIRE(C.FindCVar("editor_web_port") != nullptr);
    REQUIRE(C.FindCVar("editor_web_url") != nullptr);
    REQUIRE(C.FindCommand("mem_heatmap") != nullptr);
    REQUIRE(C.FindCommand("flightrecorder") != nullptr);
    REQUIRE(C.FindCommand("undo") != nullptr);
    REQUIRE(C.FindCommand("redo") != nullptr);
    REQUIRE(C.FindCommand("fav") != nullptr);
    REQUIRE(C.FindCommand("list_favs") != nullptr);
    REQUIRE(C.FindCommand("list_cvars") != nullptr);
    REQUIRE(C.FindCommand("list_commands") != nullptr);
    REQUIRE(C.FindCommand("help") != nullptr);
    REQUIRE(C.FindCommand("clear") != nullptr);
    REQUIRE(C.FindCommand("cvar") != nullptr);
    REQUIRE(C.FindCommand("gpu_info") != nullptr);
    REQUIRE(C.FindCommand("render_stats") != nullptr);
    REQUIRE(C.FindCommand("scene_stats") != nullptr);
    REQUIRE(C.FindCommand("script_reload") != nullptr);
    REQUIRE(C.FindCommand("web_console") != nullptr);
    REQUIRE(C.FindCommand("toggle") != nullptr);
    REQUIRE(C.FindCommand("exec") != nullptr);
    REQUIRE(C.FindCommand("defaults") != nullptr);
    REQUIRE(C.FindCommand("quit") != nullptr);
    REQUIRE(C.FindCommand("exit") != nullptr);

    REQUIRE(C.SetCVarOverride("r_console_smart_resolve", "0"));
    cn::init_runtime_console(C);
    REQUIRE(C.FindCVar("r_console_smart_resolve") == smart);
    REQUIRE(smart->value == "0");
    REQUIRE(C.FindCommand("mem_heatmap") != nullptr);
    REQUIRE(C.FindCommand("flightrecorder") != nullptr);
    REQUIRE(C.FindCommand("undo") != nullptr);
    REQUIRE(C.FindCommand("redo") != nullptr);
    REQUIRE(C.FindCommand("toggle") != nullptr);

    REQUIRE(C.SetCVarOverride("r_console_smart_resolve", "1"));
}

TEST_CASE("runtime console help and cvar command surface",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();

    auto* cv = C.RegisterCVar("test_runtime_console_cvar_cmd", "cold", "");
    REQUIRE(cv != nullptr);
    cv->allowed_values = {"cold", "hot"};
    REQUIRE(C.SetCVarOverride("test_runtime_console_cvar_cmd", "cold"));

    auto help = C.Execute("help test_runtime_console_cvar_cmd");
    REQUIRE(help.ok);
    REQUIRE(help.output.find("cvars:") != std::string::npos);
    REQUIRE(help.output.find("test_runtime_console_cvar_cmd") != std::string::npos);

    auto listed = C.Execute("cvar list test_runtime_console_cvar");
    REQUIRE(listed.ok);
    REQUIRE(listed.output.find("test_runtime_console_cvar_cmd") !=
            std::string::npos);

    auto got = C.Execute("cvar get test_runtime_console_cvar_cmd");
    REQUIRE(got.ok);
    REQUIRE(got.output.find("cold") != std::string::npos);

    auto set = C.Execute("cvar set test_runtime_console_cvar_cmd hot");
    REQUIRE(set.ok);
    REQUIRE(cv->value == "hot");

    auto toggled = C.Execute("cvar toggle test_runtime_console_cvar_cmd");
    REQUIRE(toggled.ok);
    REQUIRE(cv->value == "cold");

    REQUIRE(C.SetCVarOverride("test_runtime_console_cvar_cmd", "hot"));
    auto reset = C.Execute("cvar reset test_runtime_console_cvar_cmd");
    REQUIRE(reset.ok);
    REQUIRE(cv->value == "cold");

    cn::set_runtime_console_open(true);
    cn::update_runtime_console(C, input(false, false, false, false, "help"));
    cn::update_runtime_console(C, input(false, false, true));
    REQUIRE_FALSE(cn::runtime_console_output().empty());
    auto clear = C.Execute("clear");
    REQUIRE(clear.ok);
    REQUIRE(cn::runtime_console_output().empty());

    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
}

TEST_CASE("runtime console provider commands are late-bound",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);

    cn::set_runtime_console_gpu_info_provider({});
    auto gpu = C.Execute("gpu_info");
    REQUIRE(gpu.ok);
    REQUIRE(gpu.output.find("gpu_info: unavailable") != std::string::npos);

    cn::set_runtime_console_gpu_info_provider([] {
        return std::string{"GPU: test adapter\nRT: yes"};
    });
    gpu = C.Execute("gpu_info");
    REQUIRE(gpu.ok);
    REQUIRE(gpu.output.find("GPU: test adapter") != std::string::npos);
    REQUIRE(gpu.output.back() == '\n');

    cn::set_runtime_console_render_stats_provider([] {
        return std::string{"frames: 12"};
    });
    auto render = C.Execute("render_stats");
    REQUIRE(render.ok);
    REQUIRE(render.output.find("frames: 12") != std::string::npos);

    cn::set_runtime_console_scene_stats_provider([] {
        return std::string{"entities: 1"};
    });
    auto scene = C.Execute("scene_stats");
    REQUIRE(scene.ok);
    REQUIRE(scene.output.find("entities: 1") != std::string::npos);

    cn::set_runtime_console_script_reload_provider([] {
        return std::string{"script_reload: ok"};
    });
    auto script = C.Execute("script_reload");
    REQUIRE(script.ok);
    REQUIRE(script.output.find("script_reload: ok") != std::string::npos);

    cn::set_runtime_console_gpu_info_provider({});
    cn::set_runtime_console_render_stats_provider({});
    cn::set_runtime_console_scene_stats_provider({});
    cn::set_runtime_console_script_reload_provider({});
}

TEST_CASE("runtime console pump drains queued work",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);

    auto* cv = C.RegisterCVar("test_runtime_console_pump", "0", "");
    REQUIRE(cv != nullptr);
    REQUIRE(C.SetCVarOverride("test_runtime_console_pump", "0"));

    bool responder_called = false;
    C.QueueExecute("test_runtime_console_pump 7",
                   [&](const cn::ExecuteResult& result) {
                       responder_called = true;
                       REQUIRE(result.ok);
                   });

    cn::pump_runtime_console(C);
    REQUIRE(responder_called);
    REQUIRE(cv->value == "7");

    // Empty queue is also safe to pump every frame.
    cn::pump_runtime_console(C);
}

TEST_CASE("runtime console key state toggles and consumes escape",
          "[core][console][runtime]") {
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
    REQUIRE_FALSE(cn::consume_runtime_console_quit_requested());
    REQUIRE_FALSE(cn::runtime_console_open());

    cn::RuntimeConsoleFrame frame =
        cn::update_runtime_console(input(true, false));
    REQUIRE(frame.open);
    REQUIRE(frame.toggle_consumed);
    REQUIRE_FALSE(frame.escape_consumed);
    REQUIRE(cn::runtime_console_open());

    frame = cn::update_runtime_console(input(false, true));
    REQUIRE_FALSE(frame.open);
    REQUIRE_FALSE(frame.toggle_consumed);
    REQUIRE(frame.escape_consumed);
    REQUIRE_FALSE(cn::runtime_console_open());

    frame = cn::update_runtime_console(input(false, true));
    REQUIRE_FALSE(frame.open);
    REQUIRE_FALSE(frame.toggle_consumed);
    REQUIRE(frame.escape_consumed);
    REQUIRE(cn::consume_runtime_console_quit_requested());

    frame = cn::update_runtime_console(input(true, true));
    REQUIRE(frame.open);
    REQUIRE(frame.toggle_consumed);
    REQUIRE_FALSE(frame.escape_consumed);

    frame = cn::update_runtime_console(
        input(false, false, false, false, "`"));
    REQUIRE_FALSE(frame.open);
    REQUIRE(frame.toggle_consumed);
    REQUIRE(frame.text_consumed);
    REQUIRE_FALSE(cn::runtime_console_open());

    frame = cn::update_runtime_console(
        input(false, false, false, false, "~"));
    REQUIRE(frame.open);
    REQUIRE(frame.toggle_consumed);
    REQUIRE(frame.text_consumed);
    REQUIRE(cn::runtime_console_line().empty());

    cn::set_runtime_console_open(false);
    REQUIRE_FALSE(cn::consume_runtime_console_quit_requested());
}

TEST_CASE("runtime console line editor executes through Console",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
    C.ClearHistory();

    auto* cv = C.RegisterCVar("test_runtime_console_line", "0", "");
    REQUIRE(cv != nullptr);
    REQUIRE(C.SetCVarOverride("test_runtime_console_line", "0"));

    cn::RuntimeConsoleFrame frame =
        cn::update_runtime_console(C, input(true, false));
    REQUIRE(frame.open);
    REQUIRE(cn::runtime_console_line().empty());

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "test_runtime_console_line 42"));
    REQUIRE(frame.text_consumed);
    REQUIRE(cn::runtime_console_line() == "test_runtime_console_line 42");

    frame = cn::update_runtime_console(
        C, input(false, false, false, true));
    REQUIRE(frame.backspace_consumed);
    REQUIRE(cn::runtime_console_line() == "test_runtime_console_line 4");

    frame = cn::update_runtime_console(C, input(false, false, false, false, "2"));
    REQUIRE(frame.text_consumed);
    REQUIRE(cn::runtime_console_line() == "test_runtime_console_line 42");

    frame = cn::update_runtime_console(
        C, input(false, false, true));
    REQUIRE(frame.enter_consumed);
    REQUIRE(cn::runtime_console_line().empty());
    REQUIRE(cv->value == "42");

    const auto output = cn::runtime_console_output();
    REQUIRE_FALSE(output.empty());
    REQUIRE(output.front().find("> test_runtime_console_line 42") !=
            std::string::npos);

    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
}

TEST_CASE("runtime console line editor supports cursor selection and clipboard",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();

    std::string clipboard;
    cn::set_runtime_console_clipboard_setter(
        [&](std::string_view text) { clipboard = std::string{text}; });

    cn::RuntimeConsoleFrame frame =
        cn::update_runtime_console(C, input(true, false));
    REQUIRE(frame.open);
    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "abcdef"));
    REQUIRE(cn::runtime_console_line() == "abcdef");
    REQUIRE(cn::runtime_console_cursor() == 6);

    cn::update_runtime_console(
        C, cn::RuntimeConsoleInput{
               false, false, false, false, false, false, false, false, false,
               true, false, true, false, false, false, false, true,
               false, false, false, false,
               false, 0.0f, 0.0f, 0.0f, false, 1280.0f, 720.0f, {}, {}});
    REQUIRE(cn::runtime_console_cursor() == 5);
    REQUIRE(cn::runtime_console_selection_start() == 5);
    REQUIRE(cn::runtime_console_selection_end() == 6);

    cn::update_runtime_console(
        C, cn::RuntimeConsoleInput{
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, false, false, false, false,
               true, false, false, false,
               false, 0.0f, 0.0f, 0.0f, false, 1280.0f, 720.0f, {}, {}});
    REQUIRE(clipboard == "f");

    cn::update_runtime_console(
        C, cn::RuntimeConsoleInput{
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, false, false, false, false,
               false, false, false, true,
               false, 0.0f, 0.0f, 0.0f, false, 1280.0f, 720.0f, {}, {}});
    REQUIRE(cn::runtime_console_selection_start() == 0);
    REQUIRE(cn::runtime_console_selection_end() == 6);

    cn::update_runtime_console(
        C, cn::RuntimeConsoleInput{
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, false, false, false, false,
               false, true, false, false,
               false, 0.0f, 0.0f, 0.0f, false, 1280.0f, 720.0f, {}, {}});
    REQUIRE(clipboard == "abcdef");
    REQUIRE(cn::runtime_console_line().empty());

    cn::update_runtime_console(
        C, cn::RuntimeConsoleInput{
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, false, false, false, false,
               false, false, true, false,
               false, 0.0f, 0.0f, 0.0f, false, 1280.0f, 720.0f, clipboard, {}});
    REQUIRE(cn::runtime_console_line() == "abcdef");
    REQUIRE(cn::runtime_console_cursor() == 6);

    cn::RuntimeConsoleInput home{};
    home.edit_home_pressed = true;
    home.viewport_width = 1280.0f;
    home.viewport_height = 720.0f;
    cn::update_runtime_console(C, home);
    REQUIRE(cn::runtime_console_cursor() == 0);

    cn::RuntimeConsoleInput end{};
    end.edit_end_pressed = true;
    end.viewport_width = 1280.0f;
    end.viewport_height = 720.0f;
    cn::update_runtime_console(C, end);
    REQUIRE(cn::runtime_console_cursor() == 6);

    cn::RuntimeConsoleInput mouse_down{};
    mouse_down.mouse_left_down = true;
    mouse_down.mouse_x = 64.0f;
    mouse_down.mouse_y = 300.0f;
    mouse_down.viewport_width = 1280.0f;
    mouse_down.viewport_height = 720.0f;
    cn::update_runtime_console(C, mouse_down);

    cn::RuntimeConsoleInput mouse_drag = mouse_down;
    mouse_drag.mouse_x = 100.0f;
    cn::update_runtime_console(C, mouse_drag);
    REQUIRE(cn::runtime_console_selection_start() == 1);
    REQUIRE(cn::runtime_console_selection_end() == 4);

    cn::RuntimeConsoleInput mouse_up{};
    mouse_up.viewport_width = 1280.0f;
    mouse_up.viewport_height = 720.0f;
    cn::update_runtime_console(C, mouse_up);

    cn::set_runtime_console_clipboard_setter({});
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
}

TEST_CASE("runtime console exposes undo redo and favorites commands",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
    C.ClearFavorites();
    C.ClearHistory();

    auto* cv = C.RegisterCVar("test_runtime_console_undo", "off", "");
    REQUIRE(cv != nullptr);
    REQUIRE(C.SetCVarOverride("test_runtime_console_undo", "off"));

    cn::RuntimeConsoleFrame frame =
        cn::update_runtime_console(C, input(true, false));
    REQUIRE(frame.open);

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "test_runtime_console_undo on"));
    REQUIRE(frame.text_consumed);
    frame = cn::update_runtime_console(C, input(false, false, true));
    REQUIRE(frame.enter_consumed);
    REQUIRE(cv->value == "on");

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "undo"));
    REQUIRE(frame.text_consumed);
    frame = cn::update_runtime_console(C, input(false, false, true));
    REQUIRE(frame.enter_consumed);
    REQUIRE(cv->value == "off");

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "redo"));
    REQUIRE(frame.text_consumed);
    frame = cn::update_runtime_console(C, input(false, false, true));
    REQUIRE(frame.enter_consumed);
    REQUIRE(cv->value == "on");

    auto fav = C.Execute("fav test_runtime_console_undo off");
    REQUIRE(fav.ok);
    REQUIRE(C.FavoriteCount() >= 1);
    auto list = C.Execute("list_favs");
    REQUIRE(list.output.find("test_runtime_console_undo off") !=
            std::string::npos);

    auto* mode = C.RegisterCVar("test_runtime_console_toggle", "off", "");
    REQUIRE(mode != nullptr);
    mode->allowed_values = {"off", "on"};
    REQUIRE(C.SetCVarOverride("test_runtime_console_toggle", "off"));
    auto toggled = C.ExecuteScript("toggle test_runtime_console_toggle");
    REQUIRE(toggled.ok);
    REQUIRE(mode->value == "on");
    auto undone = C.Execute("undo");
    REQUIRE(undone.ok);
    REQUIRE(mode->value == "off");

    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
}

TEST_CASE("runtime console recalls command history",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
    C.ClearHistory();

    auto* cv = C.RegisterCVar("test_runtime_console_history", "0", "");
    REQUIRE(cv != nullptr);
    REQUIRE(C.SetCVarOverride("test_runtime_console_history", "0"));

    cn::RuntimeConsoleFrame frame =
        cn::update_runtime_console(C, input(true, false));
    REQUIRE(frame.open);

    frame = cn::update_runtime_console(
        C,
        input(false,
              false,
              false,
              false,
              "test_runtime_console_history 7",
              false,
              false));
    REQUIRE(frame.text_consumed);

    frame = cn::update_runtime_console(C, input(false, false, true));
    REQUIRE(frame.enter_consumed);
    REQUIRE(cv->value == "7");
    REQUIRE(cn::runtime_console_line().empty());

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, {}, true));
    REQUIRE(frame.history_consumed);
    REQUIRE(cn::runtime_console_line() == "test_runtime_console_history 7");

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, {}, false, true));
    REQUIRE(frame.history_consumed);
    REQUIRE(cn::runtime_console_line().empty());

    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
}

TEST_CASE("runtime console completes commands and scrolls output",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
    C.ClearHistory();

    auto* cv = C.RegisterCVar("test_runtime_console_completion", "off", "");
    REQUIRE(cv != nullptr);
    cv->allowed_values = {"off", "on"};
    REQUIRE(C.SetCVarOverride("test_runtime_console_completion", "off"));

    cn::RuntimeConsoleFrame frame =
        cn::update_runtime_console(C, input(true, false));
    REQUIRE(frame.open);

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "test_runtime_console_comp"));
    REQUIRE(frame.text_consumed);
    REQUIRE_FALSE(cn::runtime_console_completions().empty());

    if (cn::runtime_console_completions().size() > 1) {
        REQUIRE(cn::runtime_console_completion_selected() == 0);
        frame = cn::update_runtime_console(
            C, input(false, false, false, false, {}, false, true));
        REQUIRE(frame.history_consumed);
        REQUIRE(cn::runtime_console_completion_selected() == 1);
        frame = cn::update_runtime_console(
            C, input(false, false, false, false, {}, true, false));
        REQUIRE(frame.history_consumed);
        REQUIRE(cn::runtime_console_completion_selected() == 0);
    }

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "\xEF\x9C\x81"));
    REQUIRE(frame.history_consumed);
    REQUIRE(frame.text_consumed);
    if (cn::runtime_console_completions().size() > 1) {
        REQUIRE(cn::runtime_console_completion_selected() == 1);
    }
    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "\xEF\x9C\x80"));
    REQUIRE(frame.history_consumed);
    REQUIRE(frame.text_consumed);
    REQUIRE(cn::runtime_console_completion_selected() == 0);

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, {}, false, false, true));
    REQUIRE(cn::runtime_console_line() == "test_runtime_console_completion ");

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "on"));
    REQUIRE(frame.text_consumed);
    frame = cn::update_runtime_console(C, input(false, false, true));
    REQUIRE(frame.enter_consumed);
    REQUIRE(cv->value == "on");

    for (int i = 0; i < 8; ++i) {
        cn::update_runtime_console(
            C, input(false, false, false, false, "list_commands"));
        cn::update_runtime_console(C, input(false, false, true));
    }
    REQUIRE(cn::runtime_console_output_scroll() == 0);

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, {}, false, false, false, 3.0f));
    REQUIRE(cn::runtime_console_output_scroll() > 0);
    const std::size_t after_one_wheel = cn::runtime_console_output_scroll();
    REQUIRE(after_one_wheel == 1);

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, {}, false, false, false, -100.0f));
    REQUIRE(cn::runtime_console_output_scroll() == 0);

    frame = cn::update_runtime_console(
        C, cn::RuntimeConsoleInput{
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, 0.0f,
               1256.0f, 28.0f, true,
               1280.0f, 720.0f,
               {},
               {}});
    REQUIRE(cn::runtime_console_output_scroll() > 0);
    frame = cn::update_runtime_console(
        C, cn::RuntimeConsoleInput{
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, 0.0f,
               1256.0f, 275.0f, true,
               1280.0f, 720.0f,
               {},
               {}});
    REQUIRE(cn::runtime_console_output_scroll() == 0);
    cn::update_runtime_console(
        C, cn::RuntimeConsoleInput{
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, false, false, false, false, false,
               false, false, false, false, 0.0f,
               1256.0f, 275.0f, false,
               1280.0f, 720.0f,
               {},
               {}});

    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
}

TEST_CASE("runtime console arrows use history when popup is closed",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
    C.ClearHistory();

    auto* cv = C.RegisterCVar("test_runtime_console_arrow_history", "0", "");
    REQUIRE(cv != nullptr);
    REQUIRE(C.SetCVarOverride("test_runtime_console_arrow_history", "0"));

    cn::RuntimeConsoleFrame frame =
        cn::update_runtime_console(C, input(true, false));
    REQUIRE(frame.open);
    frame = cn::update_runtime_console(
        C, input(false, false, false, false,
                 "test_runtime_console_arrow_history 9"));
    REQUIRE(frame.text_consumed);
    frame = cn::update_runtime_console(C, input(false, false, true));
    REQUIRE(frame.enter_consumed);
    REQUIRE(cn::runtime_console_completions().empty());

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "\xEF\x9C\x80"));
    REQUIRE(frame.history_consumed);
    REQUIRE(frame.text_consumed);
    REQUIRE(cn::runtime_console_line() == "test_runtime_console_arrow_history 9");

    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "\xEF\x9C\x81"));
    REQUIRE(frame.history_consumed);
    REQUIRE(frame.text_consumed);
    REQUIRE(cn::runtime_console_line().empty());

    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
}

TEST_CASE("runtime console mouse click commits popup completion",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
    C.ClearHistory();

    auto* cv = C.RegisterCVar("zz_click_completion_target", "0", "");
    REQUIRE(cv != nullptr);

    cn::RuntimeConsoleFrame frame =
        cn::update_runtime_console(C, input(true, false));
    REQUIRE(frame.open);
    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "zz_click_completion_targ"));
    REQUIRE(frame.text_consumed);
    REQUIRE_FALSE(cn::runtime_console_completions().empty());

    cn::RuntimeConsoleInput mouse_down{};
    mouse_down.mouse_left_down = true;
    mouse_down.mouse_x = 48.0f;
    mouse_down.mouse_y = 266.0f;
    mouse_down.viewport_width = 1280.0f;
    mouse_down.viewport_height = 720.0f;
    frame = cn::update_runtime_console(C, mouse_down);
    REQUIRE(frame.text_consumed);
    REQUIRE(cn::runtime_console_completion_selected() == 0);

    cn::RuntimeConsoleInput mouse_up = mouse_down;
    mouse_up.mouse_left_down = false;
    frame = cn::update_runtime_console(C, mouse_up);
    REQUIRE(frame.text_consumed);
    REQUIRE(cn::runtime_console_line() == "zz_click_completion_target ");

    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
}

TEST_CASE("runtime console escape unwinds popup console then app",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
    REQUIRE_FALSE(cn::consume_runtime_console_quit_requested());

    cn::RuntimeConsoleFrame frame =
        cn::update_runtime_console(C, input(true, false));
    REQUIRE(frame.open);
    frame = cn::update_runtime_console(
        C, input(false, false, false, false, "r_console"));
    REQUIRE(frame.text_consumed);
    REQUIRE_FALSE(cn::runtime_console_completions().empty());

    frame = cn::update_runtime_console(C, input(false, true));
    REQUIRE(frame.escape_consumed);
    REQUIRE(frame.open);
    REQUIRE(cn::runtime_console_completions().empty());
    REQUIRE_FALSE(cn::consume_runtime_console_quit_requested());

    frame = cn::update_runtime_console(C, input(false, true));
    REQUIRE(frame.escape_consumed);
    REQUIRE_FALSE(frame.open);
    REQUIRE_FALSE(cn::consume_runtime_console_quit_requested());

    frame = cn::update_runtime_console(C, input(false, true));
    REQUIRE(frame.escape_consumed);
    REQUIRE_FALSE(frame.open);
    REQUIRE(cn::consume_runtime_console_quit_requested());

    cn::set_runtime_console_open(false);
    cn::clear_runtime_console_output();
}

TEST_CASE("runtime console quit aliases request shutdown",
          "[core][console][runtime]") {
    auto& C = cn::Console::Get();
    cn::init_runtime_console(C);
    REQUIRE_FALSE(cn::consume_runtime_console_quit_requested());

    auto r = C.Execute("quit");
    REQUIRE(r.ok);
    REQUIRE(cn::runtime_console_quit_requested());
    REQUIRE(cn::consume_runtime_console_quit_requested());
    REQUIRE_FALSE(cn::runtime_console_quit_requested());

    r = C.Execute("exit");
    REQUIRE(r.ok);
    REQUIRE(cn::consume_runtime_console_quit_requested());
}
