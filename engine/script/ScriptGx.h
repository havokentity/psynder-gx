// SPDX-License-Identifier: MIT
// Psynder-GX — script-lane GX extensions.  NOT part of the frozen Psynder
// public surface (Script.h).  This header adds GX-specific entry points that
// Psynder does not need:
//
//   repl_eval  — convenience free function used by the editor IPC layer
//                (lane 22) and the in-game developer console.
//
// Lane 22 should #include "engine/script/ScriptGx.h" for the repl_eval
// binding; it must NOT directly include internal/ headers.

#pragma once

#include <string>
#include <string_view>

namespace psynder::script {

// Evaluate a single Lua REPL line and return the result as a string.
//
// This is a thin wrapper around Vm::execute_repl() that converts the
// out-parameter form to a return-value form preferred by the editor IPC
// layer.  The Vm singleton must already have been started; if it has not,
// returns "(vm not started)".
//
// The input `line` is first tried as `return <expr>` (so expressions echo
// their value), then as a plain statement, matching the behaviour of the
// stock Lua REPL.  Multi-value returns are tab-separated.
//
// Thread safety: identical to Vm::execute_repl() — caller must hold the
// script-thread contract (do not call from engine workers).
[[nodiscard]] std::string repl_eval(std::string_view line);

}  // namespace psynder::script
