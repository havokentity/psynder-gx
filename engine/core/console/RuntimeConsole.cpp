// SPDX-License-Identifier: MIT
// Psynder — tiny runtime console host helpers. See RuntimeConsole.h.

#include "RuntimeConsole.h"

#include "../DiagCommands.h"
#include "Console.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace psynder::console {

namespace {

bool g_runtime_console_open = false;
bool g_runtime_console_quit_requested = false;
bool g_runtime_console_restart_requested = false;
std::string g_runtime_console_line;
std::size_t g_runtime_console_cursor = 0;
std::size_t g_runtime_console_selection_anchor = 0;
std::vector<std::string> g_runtime_console_output;
std::size_t g_runtime_console_history_cursor = 0;
std::size_t g_runtime_console_output_scroll = 0;
std::vector<CompletionMatch> g_runtime_console_completions;
std::size_t g_runtime_console_completion_selected = 0;
bool g_runtime_console_completion_selection_touched = false;
bool g_runtime_console_scroll_dragging = false;
float g_runtime_console_scroll_grab_y = 0.0f;
bool g_runtime_console_text_dragging = false;
bool g_runtime_console_popup_mouse_active = false;
std::size_t g_runtime_console_popup_mouse_index = 0;
bool g_runtime_console_mouse_left_was_down = false;
RuntimeConsoleTextProvider g_gpu_info_provider;
RuntimeConsoleTextProvider g_render_stats_provider;
RuntimeConsoleTextProvider g_scene_stats_provider;
RuntimeConsoleTextProvider g_script_reload_provider;
RuntimeConsoleClipboardSetter g_clipboard_setter;
std::string g_engine_launch_argv_json;
std::string g_engine_launch_cwd;
unsigned g_editor_ipc_port = 7655;
#if !defined(_WIN32)
pid_t g_web_console_pid = -1;
bool g_web_console_atexit_registered = false;
bool g_web_console_preserve_on_exit = false;
#endif
constexpr std::size_t kMaxOutputLines = 96;
constexpr double kRepeatDelaySeconds = 0.28;
constexpr double kRepeatIntervalSeconds = 0.045;
constexpr float kPanelPad = 12.0f;
constexpr float kLineHeight = 18.0f;
constexpr const char* kWebConsoleDefaultRoot = "engine/editor/web";
constexpr const char* kWebConsoleDefaultPort = "5176";
constexpr const char* kWebConsoleAutoUrl = "auto";
constexpr const char* kWebConsoleDefaultShell = "psyeditorgx";
constexpr const char* kWebConsoleLogPath = "/tmp/psynder-gx-web-console.log";

struct KeyRepeat {
    bool down = false;
    double next_fire = 0.0;
};
KeyRepeat g_repeat_backspace;
KeyRepeat g_repeat_prev;
KeyRepeat g_repeat_next;
KeyRepeat g_repeat_left;
KeyRepeat g_repeat_right;
KeyRepeat g_repeat_delete;

void clear_completions();

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

bool repeated_action(bool down, KeyRepeat& state, double now) {
    if (!down) {
        state.down = false;
        state.next_fire = 0.0;
        return false;
    }
    if (!state.down) {
        state.down = true;
        state.next_fire = now + kRepeatDelaySeconds;
        return true;
    }
    if (now >= state.next_fire) {
        state.next_fire = now + kRepeatIntervalSeconds;
        return true;
    }
    return false;
}

void register_core_runtime_cvars(Console& con) {
    if (con.FindCVar("r_console_smart_resolve") == nullptr) {
        auto* smart = con.RegisterCVar(
            "r_console_smart_resolve",
            "1",
            "Enable fuzzy console command/cvar resolution.",
            CVAR_ARCHIVE);
        smart->allowed_values = {
            "0", "1", "false", "true", "off", "on", "no", "yes"};
    }
    if (con.FindCVar("editor_web_root") == nullptr) {
        con.RegisterCVar(
            "editor_web_root",
            kWebConsoleDefaultRoot,
            "Path to the Psynder-GX web editor package.",
            CVAR_ARCHIVE);
    }
    if (con.FindCVar("editor_web_port") == nullptr) {
        con.RegisterCVar(
            "editor_web_port",
            kWebConsoleDefaultPort,
            "Legacy dev-server port; PsyEditorGX loads dist/ directly by default.",
            CVAR_ARCHIVE);
    }
    if (con.FindCVar("editor_web_url") == nullptr) {
        con.RegisterCVar(
            "editor_web_url",
            kWebConsoleAutoUrl,
            "Optional URL override for web_console; 'auto' loads editor dist/.",
            CVAR_ARCHIVE);
    }
    if (con.FindCVar("editor_web_shell") == nullptr) {
        auto* shell = con.RegisterCVar(
            "editor_web_shell",
            kWebConsoleDefaultShell,
            "Editor shell for web_console: psyeditorgx or system.",
            CVAR_ARCHIVE);
        shell->allowed_values = {"psyeditorgx", "system"};
    }
}

std::string join_args(std::span<const std::string_view> args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            out.push_back(' ');
        }
        out.append(args[i].data(), args[i].size());
    }
    return out;
}

void print_changes(Output& out,
                   std::string_view verb,
                   const std::vector<Console::CvarChange>& changes) {
    if (changes.empty()) {
        out.FormatLine("{}: empty", verb);
        return;
    }
    out.FormatLine("{}: {} cvar{}", verb, changes.size(),
                   changes.size() == 1 ? "" : "s");
    for (const Console::CvarChange& c : changes) {
        out.FormatLine("  {}: {} -> {}", c.name, c.from, c.to);
    }
}

bool parse_slot(std::string_view text, int& out_slot) {
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, out_slot);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

void print_cvar(Output& out, const CVar& v) {
    out.FormatLine("{} = \"{}\" (default \"{}\") -- {}",
                   v.name, v.value, v.default_value, v.description);
}

void register_cvar_command(Console& con) {
    if (con.FindCommand("cvar") != nullptr) {
        return;
    }

    con.RegisterCommand(
        "cvar",
        "Cvar tools: list/get/set/toggle/reset/reset_all.",
        [&con](std::span<const std::string_view> args, Output& out) {
            if (args.empty() || args[0] == "help") {
                out.PrintLine("usage: cvar <list|get|set|toggle|reset|reset_all> ...");
                out.PrintLine("  cvar list [prefix]");
                out.PrintLine("  cvar get <name>");
                out.PrintLine("  cvar set <name> <value>");
                out.PrintLine("  cvar toggle <name>");
                out.PrintLine("  cvar reset <name>");
                out.PrintLine("  cvar reset_all");
                return;
            }

            const std::string_view op = args[0];
            if (op == "list") {
                const std::string prefix =
                    args.size() >= 2 ? std::string{args[1]} : std::string{};
                std::size_t count = 0;
                con.EnumerateCVars(prefix, [&](CVar& v) {
                    print_cvar(out, v);
                    ++count;
                });
                if (count == 0) {
                    out.PrintLine("cvar list: no matches");
                }
                return;
            }

            if (op == "get") {
                if (args.size() != 2) {
                    out.PrintLine("usage: cvar get <name>");
                    return;
                }
                CVar* v = con.FindCVar(args[1]);
                if (!v) {
                    out.FormatLine("cvar get: unknown cvar '{}'", args[1]);
                    return;
                }
                print_cvar(out, *v);
                return;
            }

            if (op == "set") {
                if (args.size() < 3) {
                    out.PrintLine("usage: cvar set <name> <value>");
                    return;
                }
                std::string line{args[1]};
                line.push_back(' ');
                line += join_args(args.subspan(2));
                ExecuteResult r = con.ExecuteScript(line);
                if (!r.output.empty()) {
                    out.Print(r.output);
                    if (r.output.back() != '\n') {
                        out.Print("\n");
                    }
                }
                if (!r.ok) {
                    out.FormatLine("cvar set: {}", r.error);
                }
                return;
            }

            if (op == "toggle") {
                if (args.size() != 2) {
                    out.PrintLine("usage: cvar toggle <name>");
                    return;
                }
                ExecuteResult r =
                    con.ExecuteScript(fmt::format("toggle {}", args[1]));
                if (!r.output.empty()) {
                    out.Print(r.output);
                    if (r.output.back() != '\n') {
                        out.Print("\n");
                    }
                }
                if (!r.ok) {
                    out.FormatLine("cvar toggle: {}", r.error);
                }
                return;
            }

            if (op == "reset") {
                if (args.size() != 2) {
                    out.PrintLine("usage: cvar reset <name>");
                    return;
                }
                CVar* v = con.FindCVar(args[1]);
                if (!v) {
                    out.FormatLine("cvar reset: unknown cvar '{}'", args[1]);
                    return;
                }
                ExecuteResult r =
                    con.ExecuteScript(fmt::format("{} {}", v->name, v->default_value));
                if (!r.output.empty()) {
                    out.Print(r.output);
                    if (r.output.back() != '\n') {
                        out.Print("\n");
                    }
                }
                if (!r.ok) {
                    out.FormatLine("cvar reset: {}", r.error);
                }
                return;
            }

            if (op == "reset_all") {
                const std::size_t n = con.ResetAllCVarsToDefaults();
                out.FormatLine("cvar reset_all: reset {} cvar{}",
                               n, n == 1 ? "" : "s");
                return;
            }

            out.FormatLine("cvar: unknown subcommand '{}'", op);
        });
}

void print_provider_result(std::string_view name,
                           const RuntimeConsoleTextProvider& provider,
                           Output& out) {
    if (!provider) {
        out.FormatLine("{}: unavailable", name);
        return;
    }
    std::string text = provider();
    if (text.empty()) {
        out.FormatLine("{}: no data", name);
        return;
    }
    out.Print(text);
    if (text.back() != '\n') {
        out.Print("\n");
    }
}

void register_provider_command(Console& con,
                               const char* name,
                               const char* description,
                               const RuntimeConsoleTextProvider* provider) {
    if (con.FindCommand(name) != nullptr) {
        return;
    }
    con.RegisterCommand(
        name,
        description,
        [name, provider](std::span<const std::string_view>, Output& out) {
            print_provider_result(name, *provider, out);
        });
}

std::string shell_quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('\'');
    for (char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string json_string(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::string cvar_value_or(Console& con,
                          std::string_view name,
                          std::string_view fallback) {
    if (auto* v = con.FindCVar(name); v != nullptr && !v->value.empty()) {
        return v->value;
    }
    return std::string{fallback};
}

int web_console_port(Console& con) {
    if (auto* v = con.FindCVar("editor_web_port"); v != nullptr) {
        const int n = v->GetInt();
        if (n > 0 && n <= 65535) {
            return n;
        }
    }
    return 5176;
}

std::string web_console_url(Console& con) {
    const std::string configured =
        cvar_value_or(con, "editor_web_url", kWebConsoleAutoUrl);
    if (configured.empty() || configured == kWebConsoleAutoUrl) {
        return fmt::format("http://127.0.0.1:{}/", web_console_port(con));
    }
    return configured;
}

std::filesystem::path resolve_web_console_root(Console& con) {
    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("PSYNDER_GX_WEB_ROOT");
        env != nullptr && env[0] != '\0') {
        candidates.emplace_back(env);
    }
    candidates.emplace_back(
        cvar_value_or(con, "editor_web_root", kWebConsoleDefaultRoot));
    candidates.emplace_back(kWebConsoleDefaultRoot);
    candidates.emplace_back("../engine/editor/web");
    candidates.emplace_back("../../engine/editor/web");
    candidates.emplace_back("../../../engine/editor/web");

    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    for (std::filesystem::path candidate : candidates) {
        if (candidate.is_relative() && !cwd.empty()) {
            candidate = cwd / candidate;
        }
        if (file_exists(candidate / "package.json")) {
            std::filesystem::path canonical =
                std::filesystem::weakly_canonical(candidate, ec);
            return ec ? candidate.lexically_normal() : canonical;
        }
    }
    return candidates.empty() ? std::filesystem::path{kWebConsoleDefaultRoot}
                              : candidates.front();
}

std::filesystem::path resolve_web_console_dist(Console& con) {
    return resolve_web_console_root(con) / "dist";
}

bool web_console_has_custom_url(Console& con) {
    const std::string configured =
        cvar_value_or(con, "editor_web_url", kWebConsoleAutoUrl);
    return !configured.empty() && configured != kWebConsoleAutoUrl;
}

#if !defined(_WIN32)
bool web_console_process_running() {
    if (g_web_console_pid <= 0) {
        return false;
    }
    int status = 0;
    const pid_t result = ::waitpid(g_web_console_pid, &status, WNOHANG);
    if (result == 0) {
        return true;
    }
    if (result == g_web_console_pid ||
        (result < 0 && errno == ECHILD)) {
        g_web_console_pid = -1;
        return false;
    }
    return true;
}

void stop_web_console_process() {
    if (!web_console_process_running()) {
        return;
    }
    const pid_t pid = g_web_console_pid;
    ::kill(-pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            g_web_console_pid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(-pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    g_web_console_pid = -1;
}

void stop_web_console_process_at_exit() {
    if (g_web_console_preserve_on_exit) {
        return;
    }
    stop_web_console_process();
}

bool start_web_console_process(Console& con, Output& out) {
    const std::string shell_mode =
        cvar_value_or(con, "editor_web_shell", kWebConsoleDefaultShell);
    if (shell_mode == "system") {
        return true;
    }
    if (web_console_process_running()) {
        out.FormatLine("web_console: already running (pid {})",
                       static_cast<int>(g_web_console_pid));
        return true;
    }

    const std::filesystem::path root = resolve_web_console_root(con);
    if (!file_exists(root / "package.json")) {
        out.FormatLine("web_console: missing package.json under '{}'",
                       root.string());
        out.PrintLine("web_console: set editor_web_root or PSYNDER_GX_WEB_ROOT");
        return false;
    }

    const std::filesystem::path dist = resolve_web_console_dist(con);
    if (!web_console_has_custom_url(con) && !file_exists(dist / "index.html")) {
        out.FormatLine("web_console: missing built editor '{}'",
                       (dist / "index.html").string());
        out.PrintLine("web_console: run npm --prefix engine/editor/web run build");
        return false;
    }

    std::string launch_env;
    if (!g_engine_launch_argv_json.empty()) {
        launch_env += "PSYNDER_GX_ENGINE_ARGV=";
        launch_env += shell_quote(g_engine_launch_argv_json);
        launch_env += " PSYNDER_GX_ENGINE_CWD=";
        launch_env += shell_quote(g_engine_launch_cwd);
        launch_env += " PSYNDER_GX_EDITOR_IPC_PORT=";
        launch_env += std::to_string(g_editor_ipc_port);
        launch_env += " ";
    }
    std::string command;
    if (web_console_has_custom_url(con)) {
        command = fmt::format("{}npm --prefix {} run shell -- --url {}",
                              launch_env,
                              shell_quote(root.string()),
                              shell_quote(web_console_url(con)));
    } else {
        command = fmt::format("{}npm --prefix {} run shell -- --dist {}",
                              launch_env,
                              shell_quote(root.string()),
                              shell_quote(dist.string()));
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        out.PrintLine("web_console: fork failed");
        return false;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        const int in_fd = ::open("/dev/null", O_RDONLY);
        if (in_fd >= 0) {
            ::dup2(in_fd, STDIN_FILENO);
            ::close(in_fd);
        }
        const int log_fd =
            ::open(kWebConsoleLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
            ::close(log_fd);
        }
        ::execl("/bin/sh", "sh", "-lc", command.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }

    g_web_console_pid = pid;
    if (!g_web_console_atexit_registered) {
        std::atexit(stop_web_console_process_at_exit);
        g_web_console_atexit_registered = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    if (!web_console_process_running()) {
        out.FormatLine("web_console: PsyEditorGX exited early; see {}",
                       kWebConsoleLogPath);
        return false;
    }
    out.FormatLine("web_console: opened PsyEditorGX pid {}",
                   static_cast<int>(g_web_console_pid));
    out.FormatLine("web_console: log {}", kWebConsoleLogPath);
    return true;
}
#else
bool web_console_process_running() {
    return false;
}

void stop_web_console_process() {}

bool start_web_console_process(Console&, Output& out) {
    out.PrintLine("web_console: PsyEditorGX lifecycle is not wired on Windows yet");
    return false;
}
#endif

bool open_web_console_browser(Console& con, Output& out) {
    const std::string shell_mode =
        cvar_value_or(con, "editor_web_shell", kWebConsoleDefaultShell);
    if (shell_mode != "system") {
        out.PrintLine("web_console: PsyEditorGX is the active editor shell");
        return true;
    }
    const std::string configured =
        cvar_value_or(con, "editor_web_url", kWebConsoleAutoUrl);
    const bool custom_url = !configured.empty() && configured != kWebConsoleAutoUrl;
    if (custom_url) {
        // editor_web_url is a CVAR_ARCHIVE cvar settable over the local IPC
        // websocket. `open`/`xdg-open` will happily launch arbitrary scheme
        // handlers (custom app URLs, etc.), so restrict a peer-supplied URL to
        // web / file schemes before we hand it to the shell.
        const auto scheme_is = [&](std::string_view scheme) {
            if (configured.size() < scheme.size()) {
                return false;
            }
            for (usize i = 0; i < scheme.size(); ++i) {
                char c = configured[i];
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
                if (c != scheme[i]) {
                    return false;
                }
            }
            return true;
        };
        if (!scheme_is("http://") && !scheme_is("https://") && !scheme_is("file://")) {
            out.FormatLine("web_console: refusing to open non-http(s)/file URL '{}'",
                           configured);
            return false;
        }
    }
    const std::string target = custom_url
        ? configured
        : (resolve_web_console_dist(con) / "index.html").string();
    if (!custom_url && !file_exists(resolve_web_console_dist(con) / "index.html")) {
        out.FormatLine("web_console: missing built editor '{}'",
                       (resolve_web_console_dist(con) / "index.html").string());
        return false;
    }
#if defined(__APPLE__)
    const std::string command =
        fmt::format("open {} >/dev/null 2>&1 &", shell_quote(target));
#elif defined(_WIN32)
    const std::string command = fmt::format("start \"\" \"{}\"", target);
#else
    const std::string command =
        fmt::format("xdg-open {} >/dev/null 2>&1 &", shell_quote(target));
#endif
    const int rc = std::system(command.c_str());
    if (rc != 0) {
        out.FormatLine("web_console: open failed for {}", target);
        return false;
    }
    out.FormatLine("web_console: opening system browser {}", target);
    return true;
}

void print_web_console_status(Console& con, Output& out) {
    out.FormatLine("web_console: {}",
                   web_console_process_running() ? "running" : "stopped");
#if !defined(_WIN32)
    if (g_web_console_pid > 0) {
        out.FormatLine("  pid: {}", static_cast<int>(g_web_console_pid));
    }
#endif
    out.FormatLine("  root: {}", resolve_web_console_root(con).string());
    out.FormatLine("  dist: {}", resolve_web_console_dist(con).string());
    if (web_console_has_custom_url(con)) {
        out.FormatLine("  url: {}", web_console_url(con));
    }
    out.FormatLine("  shell: {}",
                   cvar_value_or(con, "editor_web_shell", kWebConsoleDefaultShell));
    out.FormatLine("  log: {}", kWebConsoleLogPath);
}

void handle_web_console_command(Console& con,
                                std::span<const std::string_view> args,
                                Output& out) {
    const std::string_view op = args.empty() ? "start" : args[0];
    if (op == "help") {
        out.PrintLine("usage: web_console [start|open|stop|status|help]");
        out.PrintLine("  web_console        open PsyEditorGX from built dist/");
        out.PrintLine("  web_console open   same as above");
        out.PrintLine("  web_console stop   close the managed PsyEditorGX shell");
        out.PrintLine("  cvars: editor_web_root editor_web_port editor_web_url editor_web_shell");
        return;
    }
    if (op == "status") {
        print_web_console_status(con, out);
        return;
    }
    if (op == "stop") {
        if (web_console_process_running()) {
            stop_web_console_process();
            out.PrintLine("web_console: stopped");
        } else {
            out.PrintLine("web_console: already stopped");
        }
        return;
    }
    if (op == "start" || op == "open") {
        if (start_web_console_process(con, out)) {
            if (open_web_console_browser(con, out)) {
                g_runtime_console_open = false;
                clear_completions();
                g_runtime_console_scroll_dragging = false;
                g_runtime_console_text_dragging = false;
                g_runtime_console_popup_mouse_active = false;
            }
        }
        return;
    }
    out.FormatLine("web_console: unknown subcommand '{}'", op);
}

void register_core_runtime_commands(Console& con) {
    auto register_quit_alias = [&con](const char* name) {
        if (con.FindCommand(name) != nullptr) {
            return;
        }
        con.RegisterCommand(
            name,
            "Request application shutdown. Optional flag: retain_web.",
            [](std::span<const std::string_view> args, Output& out) {
                g_runtime_console_quit_requested = true;
                for (const std::string_view arg : args) {
                    if (arg == "retain_web" || arg == "--retain-web" ||
                        arg == "keep_web" || arg == "--keep-web") {
#if !defined(_WIN32)
                        g_web_console_preserve_on_exit = true;
#endif
                    }
                }
                out.PrintLine("quit: requested");
            });
    };
    register_quit_alias("quit");
    register_quit_alias("exit");
    register_quit_alias("q");
    register_quit_alias("close");
    if (con.FindCommand("restart") == nullptr) {
        con.RegisterCommand(
            "restart",
            "Restart the running engine process.",
            [](std::span<const std::string_view>, Output& out) {
                g_runtime_console_restart_requested = true;
                g_runtime_console_quit_requested = true;
#if !defined(_WIN32)
                g_web_console_preserve_on_exit = true;
#endif
                out.PrintLine("restart: requested");
            });
    }

    if (con.FindCommand("help") == nullptr) {
        con.RegisterCommand(
            "help",
            "List commands and cvars. Optional prefix filters both.",
            [&con](std::span<const std::string_view> args, Output& out) {
                const std::string prefix =
                    args.empty() ? std::string{} : std::string{args[0]};
                out.PrintLine("commands:");
                con.EnumerateCommands(prefix, [&](Command& c) {
                    out.FormatLine("  {} -- {}", c.name, c.description);
                });
                out.PrintLine("cvars:");
                con.EnumerateCVars(prefix, [&](CVar& v) {
                    out.FormatLine("  {} = \"{}\" -- {}", v.name, v.value,
                                   v.description);
                });
            });
    }
    if (con.FindCommand("clear") == nullptr) {
        con.RegisterCommand(
            "clear",
            "Clear the runtime console output buffer.",
            [](std::span<const std::string_view>, Output&) {
                clear_runtime_console_output();
            });
    }
    register_cvar_command(con);
    register_provider_command(con,
                              "gpu_info",
                              "Print active GPU/backend capabilities.",
                              &g_gpu_info_provider);
    register_provider_command(con,
                              "render_stats",
                              "Print current render-frame stats.",
                              &g_render_stats_provider);
    register_provider_command(con,
                              "scene_stats",
                              "Print current scene/entity stats.",
                              &g_scene_stats_provider);
    register_provider_command(con,
                              "script_reload",
                              "Reload active scripts when the integration provides it.",
                              &g_script_reload_provider);
    if (con.FindCommand("web_console") == nullptr) {
        con.RegisterCommand(
            "web_console",
            "Start/open/stop the Psynder-GX web console workbench.",
            [&con](std::span<const std::string_view> args, Output& out) {
                handle_web_console_command(con, args, out);
            });
    }

    if (con.FindCommand("list_cvars") == nullptr) {
        con.RegisterCommand(
            "list_cvars",
            "List cvars, optionally filtered by prefix.",
            [&con](std::span<const std::string_view> args, Output& out) {
                const std::string prefix =
                    args.empty() ? std::string{} : std::string{args[0]};
                con.EnumerateCVars(prefix, [&](CVar& v) {
                    out.FormatLine("{} = \"{}\" ({})",
                                   v.name, v.value, v.description);
                });
            });
    }
    if (con.FindCommand("list_commands") == nullptr) {
        con.RegisterCommand(
            "list_commands",
            "List commands, optionally filtered by prefix.",
            [&con](std::span<const std::string_view> args, Output& out) {
                const std::string prefix =
                    args.empty() ? std::string{} : std::string{args[0]};
                con.EnumerateCommands(prefix, [&](Command& c) {
                    out.FormatLine("{} -- {}", c.name, c.description);
                });
            });
    }
    if (con.FindCommand("cvar_reset_all") == nullptr) {
        auto reset_all = [&con](std::span<const std::string_view>, Output& out) {
            const std::size_t n = con.ResetAllCVarsToDefaults();
            if (n == 0) {
                out.PrintLine("defaults: no cvars differed from default");
            } else {
                out.FormatLine("defaults: reset {} cvar{} to default",
                               n, n == 1 ? "" : "s");
            }
        };
        con.RegisterCommand(
            "cvar_reset_all",
            "Reset every cvar to its default value. Alias: defaults.",
            reset_all);
        con.RegisterCommand(
            "defaults",
            "Reset every cvar to its default value.",
            reset_all);
    }
    if (con.FindCommand("undo") == nullptr) {
        con.RegisterCommand(
            "undo",
            "Undo the last console cvar transaction.",
            [&con](std::span<const std::string_view>, Output& out) {
                print_changes(out, "undo", con.Undo());
            });
    }
    if (con.FindCommand("redo") == nullptr) {
        con.RegisterCommand(
            "redo",
            "Redo the last undone console cvar transaction.",
            [&con](std::span<const std::string_view>, Output& out) {
                print_changes(out, "redo", con.Redo());
            });
    }
    if (con.FindCommand("toggle") == nullptr) {
        con.RegisterCommand(
            "toggle",
            "Cycle a cvar through its allowed_values.",
            [&con](std::span<const std::string_view> args, Output& out) {
                if (args.size() != 1) {
                    out.PrintLine("usage: toggle <cvar>");
                    return;
                }
                CVar* v = con.FindCVar(args[0]);
                if (!v) {
                    out.FormatLine("toggle: unknown cvar '{}'", args[0]);
                    return;
                }
                if (v->allowed_values.empty()) {
                    out.FormatLine("toggle: '{}' has no allowed_values", v->name);
                    return;
                }
                std::size_t cur = v->allowed_values.size();
                for (std::size_t i = 0; i < v->allowed_values.size(); ++i) {
                    if (v->allowed_values[i] == v->value) {
                        cur = i;
                        break;
                    }
                }
                const std::size_t next =
                    (cur >= v->allowed_values.size())
                        ? 0
                        : (cur + 1u) % v->allowed_values.size();
                const std::string line =
                    fmt::format("{} {}", v->name, v->allowed_values[next]);
                ExecuteResult r = con.ExecuteScript(line);
                if (!r.output.empty()) {
                    out.Print(r.output);
                    if (r.output.back() != '\n') {
                        out.Print("\n");
                    }
                }
                if (!r.ok) {
                    out.FormatLine("toggle: {}", r.error);
                }
            });
    }
    if (con.FindCommand("exec") == nullptr) {
        con.RegisterCommand(
            "exec",
            "Run a .cfg script file.",
            [&con](std::span<const std::string_view> args, Output& out) {
                if (args.empty()) {
                    out.PrintLine("usage: exec <file>");
                    return;
                }
                ExecuteResult r = con.LoadFromFile(std::string{args[0]});
                if (!r.output.empty()) {
                    out.Print(r.output);
                    if (r.output.back() != '\n') {
                        out.Print("\n");
                    }
                }
                if (!r.ok) {
                    out.FormatLine("exec error: {}", r.error);
                }
            });
    }
    if (con.FindCommand("fav") == nullptr) {
        con.RegisterCommand(
            "fav",
            "Save a console favorite. With no args, saves the last line.",
            [&con](std::span<const std::string_view> args, Output& out) {
                std::string line = args.empty()
                    ? std::string{con.LastExecutedLine()}
                    : join_args(args);
                if (line.empty()) {
                    out.PrintLine("fav: no line to save");
                    return;
                }
                con.AddFavorite(std::move(line));
                out.FormatLine("fav: saved f{}", con.FavoriteCount());
            });
    }
    if (con.FindCommand("unfav") == nullptr) {
        con.RegisterCommand(
            "unfav",
            "Remove a 1-based console favorite slot.",
            [&con](std::span<const std::string_view> args, Output& out) {
                if (args.empty()) {
                    out.PrintLine("usage: unfav <slot>");
                    return;
                }
                int slot = 0;
                if (!parse_slot(args[0], slot) || slot <= 0 ||
                    static_cast<std::size_t>(slot) > con.FavoriteCount()) {
                    out.PrintLine("unfav: slot out of range");
                    return;
                }
                con.RemoveFavorite(static_cast<std::size_t>(slot));
                out.FormatLine("unfav: removed f{}", slot);
            });
    }
    if (con.FindCommand("fav_clear") == nullptr) {
        con.RegisterCommand(
            "fav_clear",
            "Clear all console favorites.",
            [&con](std::span<const std::string_view>, Output& out) {
                const std::size_t n = con.FavoriteCount();
                con.ClearFavorites();
                out.FormatLine("fav_clear: removed {}", n);
            });
    }
    if (con.FindCommand("list_favs") == nullptr) {
        con.RegisterCommand(
            "list_favs",
            "List console favorites.",
            [&con](std::span<const std::string_view>, Output& out) {
                const auto favs = con.Favorites();
                if (favs.empty()) {
                    out.PrintLine("list_favs: empty");
                    return;
                }
                for (std::size_t i = 0; i < favs.size(); ++i) {
                    out.FormatLine("f{}: {}", i + 1, favs[i]);
                }
            });
    }
}

void push_output(std::string line) {
    if (line.empty()) {
        return;
    }
    g_runtime_console_output.push_back(std::move(line));
    if (g_runtime_console_output.size() > kMaxOutputLines) {
        const std::size_t overflow =
            g_runtime_console_output.size() - kMaxOutputLines;
        g_runtime_console_output.erase(g_runtime_console_output.begin(),
                                       g_runtime_console_output.begin() +
                                           static_cast<std::ptrdiff_t>(overflow));
    }
    g_runtime_console_output_scroll = 0;
}

bool text_contains_toggle(std::string_view text) {
    for (const char c : text) {
        if (c == '`' || c == '~') {
            return true;
        }
    }
    return false;
}

bool text_contains_utf8(std::string_view text,
                        unsigned char a,
                        unsigned char b,
                        unsigned char c) {
    for (std::size_t i = 0; i + 2u < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == a &&
            static_cast<unsigned char>(text[i + 1u]) == b &&
            static_cast<unsigned char>(text[i + 2u]) == c) {
            return true;
        }
    }
    return false;
}

bool text_contains_arrow_up(std::string_view text) {
    // NSUpArrowFunctionKey (U+F700) when AppKit includes it in characters.
    return text_contains_utf8(text, 0xEFu, 0x9Cu, 0x80u);
}

bool text_contains_arrow_down(std::string_view text) {
    // NSDownArrowFunctionKey (U+F701) when AppKit includes it in characters.
    return text_contains_utf8(text, 0xEFu, 0x9Cu, 0x81u);
}

bool text_contains_arrow_left(std::string_view text) {
    // NSLeftArrowFunctionKey (U+F702).
    return text_contains_utf8(text, 0xEFu, 0x9Cu, 0x82u);
}

bool text_contains_arrow_right(std::string_view text) {
    // NSRightArrowFunctionKey (U+F703).
    return text_contains_utf8(text, 0xEFu, 0x9Cu, 0x83u);
}

bool text_contains_forward_delete(std::string_view text) {
    // NSDeleteFunctionKey (U+F728).
    return text_contains_utf8(text, 0xEFu, 0x9Cu, 0xA8u);
}

bool text_contains_home(std::string_view text) {
    // NSHomeFunctionKey (U+F729).
    return text_contains_utf8(text, 0xEFu, 0x9Cu, 0xA9u);
}

bool text_contains_end(std::string_view text) {
    // NSEndFunctionKey (U+F72B).
    return text_contains_utf8(text, 0xEFu, 0x9Cu, 0xABu);
}

std::string printable_ascii_excluding_toggle(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '`' || c == '~') {
            continue;
        }
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20u && u <= 0x7Eu) {
            out.push_back(c);
        }
    }
    return out;
}

bool has_printable_ascii_excluding_toggle(std::string_view text) {
    for (const char c : text) {
        if (c == '`' || c == '~') {
            continue;
        }
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20u && u <= 0x7Eu) {
            return true;
        }
    }
    return false;
}

bool selection_active() {
    return g_runtime_console_selection_anchor != g_runtime_console_cursor;
}

std::pair<std::size_t, std::size_t> selection_range() {
    return {
        std::min(g_runtime_console_selection_anchor, g_runtime_console_cursor),
        std::max(g_runtime_console_selection_anchor, g_runtime_console_cursor)};
}

void clear_selection() {
    g_runtime_console_selection_anchor = g_runtime_console_cursor;
}

void clamp_line_editor() {
    g_runtime_console_cursor =
        std::min(g_runtime_console_cursor, g_runtime_console_line.size());
    g_runtime_console_selection_anchor =
        std::min(g_runtime_console_selection_anchor,
                 g_runtime_console_line.size());
}

void reset_line_editor() {
    g_runtime_console_line.clear();
    g_runtime_console_cursor = 0;
    g_runtime_console_selection_anchor = 0;
}

void delete_selection_if_any() {
    if (!selection_active()) {
        return;
    }
    const auto [first, last] = selection_range();
    g_runtime_console_line.erase(first, last - first);
    g_runtime_console_cursor = first;
    clear_selection();
}

void insert_text_at_cursor(std::string_view text) {
    const std::string filtered = printable_ascii_excluding_toggle(text);
    if (filtered.empty()) {
        return;
    }
    delete_selection_if_any();
    g_runtime_console_line.insert(g_runtime_console_cursor, filtered);
    g_runtime_console_cursor += filtered.size();
    clear_selection();
}

void move_cursor_to(std::size_t cursor, bool extend_selection) {
    const std::size_t clamped = std::min(cursor, g_runtime_console_line.size());
    if (!extend_selection) {
        g_runtime_console_cursor = clamped;
        clear_selection();
        return;
    }
    if (!selection_active()) {
        g_runtime_console_selection_anchor = g_runtime_console_cursor;
    }
    g_runtime_console_cursor = clamped;
}

void copy_selection_to_clipboard() {
    if (!g_clipboard_setter || !selection_active()) {
        return;
    }
    const auto [first, last] = selection_range();
    g_clipboard_setter(std::string_view{g_runtime_console_line}.substr(
        first, last - first));
}

void rebuild_completions(bool force_show = false) {
    g_runtime_console_completions.clear();
    g_runtime_console_completion_selected = 0;
    g_runtime_console_completion_selection_touched = false;
    if (!g_runtime_console_open) {
        return;
    }

    clamp_line_editor();
    const TokenInfo token =
        CurrentToken(g_runtime_console_line, g_runtime_console_cursor);
    if (!force_show && token.text.empty() && token.is_token0) {
        return;
    }

    g_runtime_console_completions =
        BuildCompletions(token, /*max_results=*/12, /*description_clip=*/64);
}

void clear_completions() {
    g_runtime_console_completions.clear();
    g_runtime_console_completion_selected = 0;
    g_runtime_console_completion_selection_touched = false;
}

struct ScrollbarGeom {
    bool valid = false;
    float rail_x = 0.0f;
    float rail_y = 0.0f;
    float rail_h = 0.0f;
    float thumb_y = 0.0f;
    float thumb_h = 0.0f;
};

struct LineEditorGeom {
    bool valid = false;
    float text_x = 0.0f;
    float text_y = 0.0f;
    float text_h = 0.0f;
    std::size_t visible_start = 0;
    std::size_t visible_count = 0;
};

struct CompletionPopupGeom {
    bool valid = false;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float row_h = 0.0f;
    std::size_t first = 0;
    std::size_t rows = 0;
};

LineEditorGeom line_editor_geom(float width, float height) {
    LineEditorGeom g{};
    if (width <= 0.0f || height <= 0.0f) {
        return g;
    }
    const float panel_w = std::max(320.0f, width - 32.0f);
    const float panel_h = std::clamp(height * 0.42f, 180.0f, 360.0f);
    const float panel_x = 16.0f;
    const float panel_y = 16.0f;
    const float prompt_h = 30.0f;
    const float prompt_y = panel_y + panel_h - prompt_h - 1.0f;
    constexpr float kCellW = 12.0f;
    constexpr float kPromptGlyphs = 2.0f;
    constexpr std::size_t kMaxConsoleChars = 96;
    g.text_x = panel_x + kPanelPad + kPromptGlyphs * kCellW;
    g.text_y = prompt_y + 6.0f;
    g.text_h = 22.0f;
    g.visible_count = kMaxConsoleChars - 2u;
    g.visible_start =
        (g_runtime_console_cursor > g.visible_count)
            ? (g_runtime_console_cursor - g.visible_count)
            : 0u;
    g.valid = true;
    (void)panel_w;
    return g;
}

CompletionPopupGeom completion_popup_geom(float width, float height) {
    CompletionPopupGeom g{};
    if (width <= 0.0f || height <= 0.0f ||
        g_runtime_console_completions.empty()) {
        return g;
    }

    const float panel_w = std::max(320.0f, width - 32.0f);
    const float panel_h = std::clamp(height * 0.42f, 180.0f, 360.0f);
    const float panel_x = 16.0f;
    const float panel_y = 16.0f;
    const float prompt_h = 30.0f;
    const float prompt_y = panel_y + panel_h - prompt_h - 1.0f;
    g.rows = std::min<std::size_t>(8, g_runtime_console_completions.size());
    g.row_h = kLineHeight + 4.0f;
    const float popup_h = static_cast<float>(g.rows) * g.row_h + 6.0f;
    g.w = std::min(panel_w - 2.0f * kPanelPad, 720.0f);
    g.x = panel_x + kPanelPad;
    g.y = std::max(panel_y + kPanelPad, prompt_y - popup_h - 4.0f);
    const std::size_t selected = std::min(g_runtime_console_completion_selected,
                                          g_runtime_console_completions.size() - 1u);
    if (selected >= g.rows) {
        g.first = selected - g.rows + 1u;
    }
    g.valid = true;
    return g;
}

std::size_t cursor_from_mouse_x(float mouse_x, const LineEditorGeom& g) {
    if (!g.valid || g.visible_count == 0u) {
        return g_runtime_console_cursor;
    }
    constexpr float kCellW = 12.0f;
    const float rel = std::max(0.0f, mouse_x - g.text_x);
    const std::size_t local =
        static_cast<std::size_t>(std::floor((rel + kCellW * 0.5f) / kCellW));
    return std::min(g.visible_start + local, g_runtime_console_line.size());
}

ScrollbarGeom scrollbar_geom(float width, float height) {
    ScrollbarGeom g{};
    if (width <= 0.0f || height <= 0.0f ||
        g_runtime_console_output.size() <= 1u) {
        return g;
    }
    const float panel_w = std::max(320.0f, width - 32.0f);
    const float panel_h = std::clamp(height * 0.42f, 180.0f, 360.0f);
    const float panel_x = 16.0f;
    const float panel_y = 16.0f;
    const float prompt_h = 30.0f;
    const float prompt_y = panel_y + panel_h - prompt_h - 1.0f;
    g.rail_h = prompt_y - panel_y - 2.0f * kPanelPad;
    if (g.rail_h <= 16.0f) {
        return g;
    }
    g.rail_x = panel_x + panel_w - 8.0f;
    g.rail_y = panel_y + kPanelPad;

    const float visible_rows = std::max(1.0f, std::floor(g.rail_h / kLineHeight));
    const float total_rows = static_cast<float>(g_runtime_console_output.size());
    g.thumb_h = std::clamp(g.rail_h * (visible_rows / total_rows),
                           12.0f,
                           g.rail_h);
    const std::size_t max_scroll = g_runtime_console_output.size() - 1u;
    const float t = 1.0f -
        (static_cast<float>(std::min(g_runtime_console_output_scroll, max_scroll)) /
         static_cast<float>(max_scroll));
    g.thumb_y = g.rail_y + t * (g.rail_h - g.thumb_h);
    g.valid = true;
    return g;
}

void set_output_scroll_from_thumb_y(float thumb_y, const ScrollbarGeom& g) {
    if (!g.valid || g_runtime_console_output.size() <= 1u) {
        return;
    }
    const float range = std::max(1.0f, g.rail_h - g.thumb_h);
    const float t = std::clamp((thumb_y - g.rail_y) / range, 0.0f, 1.0f);
    const std::size_t max_scroll = g_runtime_console_output.size() - 1u;
    const float scroll = (1.0f - t) * static_cast<float>(max_scroll);
    g_runtime_console_output_scroll =
        static_cast<std::size_t>(std::lround(scroll));
}

void commit_completion() {
    if (g_runtime_console_completions.empty()) {
        rebuild_completions(/*force_show=*/true);
    }
    if (g_runtime_console_completions.empty()) {
        return;
    }

    if (g_runtime_console_completion_selected >=
        g_runtime_console_completions.size()) {
        g_runtime_console_completion_selected = 0;
    }

    const TokenInfo token =
        CurrentToken(g_runtime_console_line, g_runtime_console_cursor);
    const CompletionMatch& match =
        g_runtime_console_completions[g_runtime_console_completion_selected];
    std::string replacement = match.name;
    if (token.is_token0) {
        replacement.push_back(' ');
    }

    g_runtime_console_line.replace(token.start,
                                   token.end - token.start,
                                   replacement);
    g_runtime_console_cursor = token.start + replacement.size();
    clear_selection();
    g_runtime_console_completion_selection_touched = false;
    rebuild_completions(/*force_show=*/false);
}

void move_completion_selection(int delta) {
    if (g_runtime_console_completions.empty()) {
        return;
    }
    const std::size_t n = g_runtime_console_completions.size();
    const std::ptrdiff_t cur =
        static_cast<std::ptrdiff_t>(g_runtime_console_completion_selected);
    const std::ptrdiff_t next =
        (cur + static_cast<std::ptrdiff_t>(delta) + static_cast<std::ptrdiff_t>(n)) %
        static_cast<std::ptrdiff_t>(n);
    g_runtime_console_completion_selected = static_cast<std::size_t>(next);
    g_runtime_console_completion_selection_touched = true;
}

bool update_completion_popup_mouse(const RuntimeConsoleInput& input) {
    const bool pressed_now =
        input.mouse_left_down && !g_runtime_console_mouse_left_was_down;
    const bool released_now =
        !input.mouse_left_down && g_runtime_console_mouse_left_was_down;
    if (g_runtime_console_completions.empty()) {
        g_runtime_console_popup_mouse_active = false;
        return false;
    }

    const CompletionPopupGeom g =
        completion_popup_geom(input.viewport_width, input.viewport_height);
    if (!g.valid) {
        g_runtime_console_popup_mouse_active = false;
        return false;
    }

    const bool over_popup =
        input.mouse_x >= g.x &&
        input.mouse_x <= g.x + g.w &&
        input.mouse_y >= g.y &&
        input.mouse_y <= g.y + static_cast<float>(g.rows) * g.row_h + 6.0f;
    std::size_t hovered = g_runtime_console_completions.size();
    if (over_popup && input.mouse_y >= g.y + 2.0f) {
        const float rel_y = input.mouse_y - (g.y + 4.0f);
        if (rel_y >= -2.0f) {
            const std::size_t row =
                static_cast<std::size_t>(std::floor(std::max(0.0f, rel_y) / g.row_h));
            if (row < g.rows && g.first + row < g_runtime_console_completions.size()) {
                hovered = g.first + row;
            }
        }
    }

    if (pressed_now && hovered < g_runtime_console_completions.size()) {
        g_runtime_console_popup_mouse_active = true;
        g_runtime_console_popup_mouse_index = hovered;
        g_runtime_console_completion_selected = hovered;
        g_runtime_console_completion_selection_touched = true;
        return true;
    }

    if (input.mouse_left_down && g_runtime_console_popup_mouse_active) {
        if (hovered < g_runtime_console_completions.size()) {
            g_runtime_console_popup_mouse_index = hovered;
            g_runtime_console_completion_selected = hovered;
            g_runtime_console_completion_selection_touched = true;
        }
        return true;
    }

    if (released_now && g_runtime_console_popup_mouse_active) {
        const bool commit =
            hovered == g_runtime_console_popup_mouse_index &&
            hovered < g_runtime_console_completions.size();
        g_runtime_console_popup_mouse_active = false;
        if (commit) {
            g_runtime_console_completion_selected = hovered;
            g_runtime_console_completion_selection_touched = true;
            commit_completion();
        }
        return true;
    }

    if (!input.mouse_left_down) {
        g_runtime_console_popup_mouse_active = false;
    }
    return over_popup;
}

bool enter_should_commit_completion() {
    if (g_runtime_console_completions.empty()) {
        return false;
    }
    if (g_runtime_console_completion_selection_touched) {
        return true;
    }
    const TokenInfo token =
        CurrentToken(g_runtime_console_line, g_runtime_console_cursor);
    const CompletionMatch& match =
        g_runtime_console_completions[
            std::min(g_runtime_console_completion_selected,
                     g_runtime_console_completions.size() - 1u)];
    return match.name != token.text;
}

void apply_output_scroll(float delta) {
    if (delta == 0.0f || g_runtime_console_output.empty()) {
        return;
    }
    const int steps = (delta > 0.0f) ? 1 : -1;
    const std::size_t max_scroll = g_runtime_console_output.size() - 1u;
    if (steps > 0) {
        g_runtime_console_output_scroll = std::min(
            max_scroll,
            g_runtime_console_output_scroll + static_cast<std::size_t>(steps));
    } else {
        const std::size_t down = static_cast<std::size_t>(-steps);
        g_runtime_console_output_scroll =
            (down >= g_runtime_console_output_scroll)
                ? 0
                : (g_runtime_console_output_scroll - down);
    }
}

void update_output_scroll_drag(const RuntimeConsoleInput& input) {
    if (!input.mouse_left_down || g_runtime_console_text_dragging ||
        g_runtime_console_output.size() <= 1u) {
        g_runtime_console_scroll_dragging = false;
        return;
    }
    const ScrollbarGeom g =
        scrollbar_geom(input.viewport_width, input.viewport_height);
    if (!g.valid) {
        g_runtime_console_scroll_dragging = false;
        return;
    }

    const bool over_thumb =
        input.mouse_x >= g.rail_x - 8.0f &&
        input.mouse_x <= g.rail_x + 8.0f &&
        input.mouse_y >= g.thumb_y &&
        input.mouse_y <= g.thumb_y + g.thumb_h;
    const bool over_rail =
        input.mouse_x >= g.rail_x - 8.0f &&
        input.mouse_x <= g.rail_x + 8.0f &&
        input.mouse_y >= g.rail_y &&
        input.mouse_y <= g.rail_y + g.rail_h;

    if (!g_runtime_console_scroll_dragging) {
        if (!over_rail) {
            return;
        }
        g_runtime_console_scroll_dragging = true;
        g_runtime_console_scroll_grab_y =
            over_thumb ? (input.mouse_y - g.thumb_y) : (g.thumb_h * 0.5f);
    }
    set_output_scroll_from_thumb_y(input.mouse_y - g_runtime_console_scroll_grab_y,
                                   g);
}

void update_line_editor_drag(const RuntimeConsoleInput& input) {
    const bool pressed_now =
        input.mouse_left_down && !g_runtime_console_mouse_left_was_down;
    if (!input.mouse_left_down) {
        g_runtime_console_text_dragging = false;
        return;
    }
    if (g_runtime_console_scroll_dragging) {
        return;
    }

    const LineEditorGeom g =
        line_editor_geom(input.viewport_width, input.viewport_height);
    if (!g.valid) {
        return;
    }
    const bool over_line =
        input.mouse_y >= g.text_y - 4.0f &&
        input.mouse_y <= g.text_y + g.text_h &&
        input.mouse_x >= g.text_x - 4.0f &&
        input.mouse_x <= g.text_x + static_cast<float>(g.visible_count) * 12.0f;

    if (pressed_now && over_line) {
        g_runtime_console_text_dragging = true;
        g_runtime_console_cursor = cursor_from_mouse_x(input.mouse_x, g);
        clear_selection();
        return;
    }
    if (g_runtime_console_text_dragging) {
        if (!selection_active()) {
            g_runtime_console_selection_anchor = g_runtime_console_cursor;
        }
        g_runtime_console_cursor = cursor_from_mouse_x(input.mouse_x, g);
    }
}

void recall_history_prev(Console& con) {
    const std::vector<std::string> history = con.History();
    if (history.empty()) {
        return;
    }
    if (g_runtime_console_history_cursor == 0) {
        g_runtime_console_history_cursor = 0;
    } else {
        --g_runtime_console_history_cursor;
    }
    g_runtime_console_line = history[g_runtime_console_history_cursor];
    g_runtime_console_cursor = g_runtime_console_line.size();
    clear_selection();
}

void recall_history_next(Console& con) {
    const std::vector<std::string> history = con.History();
    if (history.empty()) {
        return;
    }
    if (g_runtime_console_history_cursor + 1 >= history.size()) {
        g_runtime_console_history_cursor = history.size();
        g_runtime_console_line.clear();
    } else {
        ++g_runtime_console_history_cursor;
        g_runtime_console_line = history[g_runtime_console_history_cursor];
    }
    g_runtime_console_cursor = g_runtime_console_line.size();
    clear_selection();
}

}  // namespace

void init_runtime_console() {
    init_runtime_console(Console::Get());
}

void init_runtime_console(Console& con) {
    register_core_runtime_cvars(con);
    register_core_runtime_commands(con);
    diag::register_console_commands(con);
    g_runtime_console_history_cursor = con.History().size();
}

void pump_runtime_console() {
    pump_runtime_console(Console::Get());
}

void pump_runtime_console(Console& con) {
    con.Drain();
}

void submit_runtime_console_line(std::string_view line) {
    submit_runtime_console_line(Console::Get(), line);
}

void submit_runtime_console_line(Console& con, std::string_view line) {
    if (line.empty()) {
        return;
    }
    const std::string submitted{line};
    push_output(std::string{"> "} + submitted);
    con.PushHistory(submitted);
    ExecuteResult result = con.ExecuteScript(submitted);
    if (!result.output.empty()) {
        push_output(result.output);
    }
}

RuntimeConsoleFrame update_runtime_console(const RuntimeConsoleInput& input) {
    return update_runtime_console(Console::Get(), input);
}

RuntimeConsoleFrame update_runtime_console(Console& con,
                                           const RuntimeConsoleInput& input) {
    RuntimeConsoleFrame out{};

    const bool text_toggle = text_contains_toggle(input.text);
    if (input.toggle_pressed || text_toggle) {
        g_runtime_console_open = !g_runtime_console_open;
        out.toggle_consumed = true;
        out.text_consumed = text_toggle;
        g_runtime_console_output_scroll = 0;
        g_runtime_console_scroll_dragging = false;
        g_runtime_console_text_dragging = false;
        g_repeat_backspace = {};
        g_repeat_prev = {};
        g_repeat_next = {};
        g_repeat_left = {};
        g_repeat_right = {};
        g_repeat_delete = {};
        rebuild_completions(/*force_show=*/false);
    } else if (input.escape_pressed) {
        if (g_runtime_console_open) {
            if (!g_runtime_console_completions.empty()) {
                clear_completions();
            } else {
                g_runtime_console_open = false;
            }
        } else {
            g_runtime_console_quit_requested = true;
        }
        out.escape_consumed = true;
    }

    if (g_runtime_console_open && !out.toggle_consumed) {
        const double now = now_seconds();
        const bool text_prev = text_contains_arrow_up(input.text);
        const bool text_next = text_contains_arrow_down(input.text);
        const bool text_left = text_contains_arrow_left(input.text);
        const bool text_right = text_contains_arrow_right(input.text);
        const bool text_delete = text_contains_forward_delete(input.text);
        const bool text_home = text_contains_home(input.text);
        const bool text_end = text_contains_end(input.text);
        const bool prev_action =
            repeated_action(input.history_prev_down || input.history_prev_pressed ||
                                text_prev,
                            g_repeat_prev,
                            now);
        const bool next_action =
            repeated_action(input.history_next_down || input.history_next_pressed ||
                                text_next,
                            g_repeat_next,
                            now);
        const bool backspace_action =
            repeated_action(input.backspace_down || input.backspace_pressed,
                            g_repeat_backspace,
                            now);
        const bool left_action =
            repeated_action(input.edit_left_down || input.edit_left_pressed ||
                                text_left,
                            g_repeat_left,
                            now);
        const bool right_action =
            repeated_action(input.edit_right_down || input.edit_right_pressed ||
                                text_right,
                            g_repeat_right,
                            now);
        const bool delete_action =
            repeated_action(input.edit_delete_pressed || text_delete,
                            g_repeat_delete,
                            now);

        apply_output_scroll(input.scroll_delta);
        const bool popup_mouse_consumed = update_completion_popup_mouse(input);
        if (!popup_mouse_consumed) {
            update_output_scroll_drag(input);
            update_line_editor_drag(input);
        } else {
            g_runtime_console_scroll_dragging = false;
            g_runtime_console_text_dragging = false;
            out.text_consumed = true;
        }

        if (input.copy_pressed) {
            copy_selection_to_clipboard();
        }
        if (input.cut_pressed) {
            copy_selection_to_clipboard();
            delete_selection_if_any();
            rebuild_completions(/*force_show=*/false);
        }
        if (input.select_all_pressed && !g_runtime_console_line.empty()) {
            g_runtime_console_selection_anchor = 0;
            g_runtime_console_cursor = g_runtime_console_line.size();
        }
        if (input.paste_pressed && !input.paste_text.empty()) {
            insert_text_at_cursor(input.paste_text);
            g_runtime_console_history_cursor = con.History().size();
            out.text_consumed = true;
            rebuild_completions(/*force_show=*/false);
        }
        if (input.edit_home_pressed || text_home) {
            move_cursor_to(0, input.shift_down);
            out.text_consumed = true;
        }
        if (input.edit_end_pressed || text_end) {
            move_cursor_to(g_runtime_console_line.size(), input.shift_down);
            out.text_consumed = true;
        }
        if (left_action) {
            move_cursor_to(g_runtime_console_cursor > 0
                               ? g_runtime_console_cursor - 1u
                               : 0u,
                           input.shift_down);
            out.text_consumed = true;
        }
        if (right_action) {
            move_cursor_to(g_runtime_console_cursor + 1u, input.shift_down);
            out.text_consumed = true;
        }

        if (input.tab_pressed) {
            commit_completion();
            out.text_consumed = true;
        } else if (input.enter_pressed && enter_should_commit_completion()) {
            commit_completion();
            out.enter_consumed = true;
            out.text_consumed = true;
        } else if (prev_action || next_action) {
            if (!g_runtime_console_completions.empty() && prev_action) {
                move_completion_selection(-1);
                out.history_consumed = true;
            } else if (!g_runtime_console_completions.empty() && next_action) {
                move_completion_selection(+1);
                out.history_consumed = true;
            } else if (prev_action) {
                recall_history_prev(con);
                out.history_consumed = true;
            } else {
                recall_history_next(con);
                out.history_consumed = true;
            }
        }
        if (text_prev || text_next || text_left || text_right || text_delete ||
            text_home || text_end || input.edit_home_pressed ||
            input.edit_end_pressed || input.edit_delete_pressed ||
            g_runtime_console_text_dragging) {
            out.text_consumed = true;
        }
        if (has_printable_ascii_excluding_toggle(input.text)) {
            insert_text_at_cursor(input.text);
            g_runtime_console_history_cursor = con.History().size();
            out.text_consumed = true;
            rebuild_completions(/*force_show=*/false);
        }
        if (backspace_action) {
            if (selection_active()) {
                delete_selection_if_any();
            } else if (g_runtime_console_cursor > 0) {
                g_runtime_console_line.erase(g_runtime_console_cursor - 1u, 1u);
                --g_runtime_console_cursor;
                clear_selection();
            }
            g_runtime_console_history_cursor = con.History().size();
            out.backspace_consumed = true;
            rebuild_completions(/*force_show=*/false);
        }
        if (delete_action) {
            if (selection_active()) {
                delete_selection_if_any();
            } else if (g_runtime_console_cursor < g_runtime_console_line.size()) {
                g_runtime_console_line.erase(g_runtime_console_cursor, 1u);
            }
            g_runtime_console_history_cursor = con.History().size();
            out.text_consumed = true;
            rebuild_completions(/*force_show=*/false);
        }
        if (input.enter_pressed && !out.enter_consumed) {
            if (!g_runtime_console_line.empty()) {
                push_output(std::string{"> "} + g_runtime_console_line);
                con.PushHistory(g_runtime_console_line);
                g_runtime_console_history_cursor = con.History().size();
                ExecuteResult result = con.ExecuteScript(g_runtime_console_line);
                if (!result.output.empty()) {
                    push_output(result.output);
                }
                reset_line_editor();
            }
            clear_completions();
            out.enter_consumed = true;
        }
    }

    g_runtime_console_mouse_left_was_down = input.mouse_left_down;
    out.open = g_runtime_console_open;
    return out;
}

bool runtime_console_open() {
    return g_runtime_console_open;
}

void set_runtime_console_open(bool open) {
    g_runtime_console_open = open;
    if (!open) {
        clear_completions();
        g_runtime_console_scroll_dragging = false;
        g_runtime_console_text_dragging = false;
        reset_line_editor();
        g_repeat_backspace = {};
        g_repeat_prev = {};
        g_repeat_next = {};
        g_repeat_left = {};
        g_repeat_right = {};
        g_repeat_delete = {};
    }
}

bool runtime_console_quit_requested() {
    return g_runtime_console_quit_requested;
}

bool consume_runtime_console_quit_requested() {
    const bool requested = g_runtime_console_quit_requested;
    g_runtime_console_quit_requested = false;
    return requested;
}

bool runtime_console_restart_requested() {
    return g_runtime_console_restart_requested;
}

bool consume_runtime_console_restart_requested() {
    const bool requested = g_runtime_console_restart_requested;
    g_runtime_console_restart_requested = false;
    return requested;
}

std::string_view runtime_console_line() {
    return g_runtime_console_line;
}

std::size_t runtime_console_cursor() {
    return std::min(g_runtime_console_cursor, g_runtime_console_line.size());
}

std::size_t runtime_console_selection_start() {
    return selection_range().first;
}

std::size_t runtime_console_selection_end() {
    return selection_range().second;
}

std::span<const std::string> runtime_console_output() {
    return g_runtime_console_output;
}

std::size_t runtime_console_output_scroll() {
    return g_runtime_console_output_scroll;
}

std::span<const CompletionMatch> runtime_console_completions() {
    return g_runtime_console_completions;
}

std::size_t runtime_console_completion_selected() {
    return g_runtime_console_completion_selected;
}

void clear_runtime_console_output() {
    g_runtime_console_output.clear();
    g_runtime_console_output_scroll = 0;
}

void set_runtime_console_gpu_info_provider(RuntimeConsoleTextProvider provider) {
    g_gpu_info_provider = std::move(provider);
}

void set_runtime_console_render_stats_provider(RuntimeConsoleTextProvider provider) {
    g_render_stats_provider = std::move(provider);
}

void set_runtime_console_scene_stats_provider(RuntimeConsoleTextProvider provider) {
    g_scene_stats_provider = std::move(provider);
}

void set_runtime_console_script_reload_provider(RuntimeConsoleTextProvider provider) {
    g_script_reload_provider = std::move(provider);
}

void set_runtime_console_clipboard_setter(RuntimeConsoleClipboardSetter setter) {
    g_clipboard_setter = std::move(setter);
}

void set_runtime_console_engine_launch(int argc, char** argv) {
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        g_engine_launch_argv_json.clear();
        g_engine_launch_cwd.clear();
        return;
    }

    std::string json = "[";
    for (int i = 0; i < argc; ++i) {
        if (i > 0) {
            json.push_back(',');
        }
        json += json_string(argv[i] != nullptr ? std::string_view{argv[i]} : std::string_view{});
    }
    json.push_back(']');
    g_engine_launch_argv_json = std::move(json);

    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    g_engine_launch_cwd = ec ? std::string{"."} : cwd.string();
}

void set_runtime_console_editor_ipc_port(unsigned port) {
    if (port > 0 && port <= 65535) {
        g_editor_ipc_port = port;
    }
}

}  // namespace psynder::console
