// SPDX-License-Identifier: MIT
// Psynder-GX — REPL host implementation. Bridges the Lua VM to the engine
// console (psynder::console::Console):
//
//   * Lua `print(...)` is overridden to route through a sink. The default
//     sink forwards to the engine log (so script output shows up in the
//     console / log stream); during a `lua` console command the sink is
//     temporarily redirected so the printed text is captured into that
//     command's Output.
//   * A `lua` console command evaluates its argument as a Lua REPL line
//     against the live Vm and writes the result (or error) to the console.
//
// Single-thread contract: the VM runs on the dedicated game thread, so the
// sink pointer needs no locking (matches GxCvars.cpp's assumption).

#include "Repl.h"

#include "core/Log.h"
#include "core/console/Console.h"
#include "script/Script.h"

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lauxlib.h"
#include "lua.h"
}
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#include <atomic>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace psynder::script::detail {

namespace {

// When non-null, Lua print() output is appended here instead of the log.
// Set only for the duration of a `lua` console command (save/restore so
// nested invocations behave). Single-thread VM -> no synchronisation needed.
::psynder::console::Output* g_capture = nullptr;

// The `lua` console command is registered once for the process; Console
// outlives any number of Vm start/shutdown cycles.
std::atomic<bool> g_console_registered{false};

// Overridden Lua `print`: gather all arguments (honouring __tostring) into a
// single tab-separated line and hand it to the active sink.
int l_print(lua_State* L) {
    const int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) {
            line += '\t';
        }
        std::size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);  // pushes a string copy
        line.append(s, len);
        lua_pop(L, 1);
    }
    if (g_capture) {
        g_capture->PrintLine(line);
    } else {
        PSY_LOG_INFO("[lua] {}", line);
    }
    return 0;
}

// `lua <line>` console command. Re-joins the tokenised arguments with single
// spaces (the console tokenizer split them) and evaluates the result against
// the live Vm, capturing any print() output into `out` alongside the value.
void lua_console_cmd(std::span<const std::string_view> args,
                     ::psynder::console::Output& out) {
    if (args.empty()) {
        out.PrintLine("usage: lua <expression-or-statement>");
        return;
    }
    std::string line;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            line += ' ';
        }
        line.append(args[i]);
    }

    ::psynder::console::Output* prev = g_capture;
    g_capture = &out;
    std::string result;
    const bool ok = ::psynder::script::Vm::Get().execute_repl(line, result);
    g_capture = prev;

    if (!ok) {
        out.FormatLine("error: {}", result);
    } else if (!result.empty()) {
        out.PrintLine(result);
    }
}

}  // namespace

void install_repl(lua_State* L) {
    // Replace the stock base-library print with our console-routing version.
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    // Register the developer-console entry point exactly once.
    if (!g_console_registered.exchange(true)) {
        ::psynder::console::Console::Get().RegisterCommand(
            "lua", "Evaluate a Lua REPL line against the live script VM",
            lua_console_cmd);
    }
}

void uninstall_repl() {
    // The lua_State is about to close; make sure a stray print() can never
    // dereference a console Output that has gone away.
    g_capture = nullptr;
}

}  // namespace psynder::script::detail
