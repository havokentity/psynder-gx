// SPDX-License-Identifier: MIT
// Psynder-GX — script-lane REPL host. Wires the Lua VM into the engine
// console: overrides Lua `print` so its output reaches the console, and
// registers a `lua` console command that evaluates a line against the live
// Vm (see Vm::execute_repl). NOT a public header — only the script lane's
// .cpp files include this.

#pragma once

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lua.h"
}
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

namespace psynder::script::detail {

// Called from Vm::start() on the fresh lua_State. Installs the `print`
// override (routes to the engine log by default) and, once per process,
// registers the `lua` console command with psynder::console::Console.
void install_repl(lua_State* L);

// Called from Vm::shutdown(). Detaches any transient console-capture sink so
// a later print() cannot touch a destroyed console Output. Safe if install_repl
// was never called.
void uninstall_repl();

}  // namespace psynder::script::detail
