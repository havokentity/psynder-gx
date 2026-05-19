// SPDX-License-Identifier: MIT
// Psynder-GX script lane — GX-specific cvar bindings.
//
// Registers the three GX renderer/gameplay console variables that Lua scripts
// need to read and write:
//
//   r_upscaler    (string)  — "off" | "dlss" | "fsr" | "xess" | "metalfx"
//   r_dynamic_res (float)   — dynamic-resolution scale, range [0.5, 1.0]
//   cl_tickrate   (int)     — client simulation tickrate, range [20, 128]
//
// These cvars are **owned by lane 20 (script)** for their Lua-side exposure;
// the GPU renderer (lane 09/11) and the network stack (lane 18) are expected
// to call `psynder::console::Console::Get().FindCVar(...)` to read them on
// their side. No circular include: lane 20 depends on core/console, not on
// lane 09 / 11 / 18 headers.
//
// Untested in vendored Psynder code (Wave B audit, 2026-05-19):
//   - `on_change` callbacks fired across Lua GC cycles: the lambda captures a
//     raw lua_State* from Vm's pimpl. If Vm::shutdown() runs before the CVar
//     is destroyed (Console outlives Vm), the callback fires against a dead
//     state. Mitigation: callbacks deregistered in install_gx_cvars_on_close().
//     This path is NOT tested.
//   - CVar persistence (CVAR_ARCHIVE): r_upscaler + r_dynamic_res are archived;
//     LoadFromFile round-trip via Lua has NOT been exercised. If the cfg file
//     contains an out-of-range r_dynamic_res value, the clamping in the setter
//     fires but the cvar's stored string stays as-is (Console stores strings,
//     not floats). The discrepancy is not tested.

#pragma once

#include "core/Types.h"

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

namespace psynder::script::detail {

// Called once from Vm::start() after install_world_api(). Registers the three
// GX cvars with Console::Get() and installs a `gx` table on `L` with Lua
// getter / setter bindings:
//
//   gx.r_upscaler()              -> string
//   gx.set_r_upscaler(val)       -> nil  (validates against allowed list)
//   gx.r_dynamic_res()           -> number
//   gx.set_r_dynamic_res(val)    -> nil  (clamps to [0.5, 1.0])
//   gx.cl_tickrate()             -> integer
//   gx.set_cl_tickrate(val)      -> nil  (clamps to [20, 128])
//
// Cvars are **also** accessible by name via the existing console command
// interface (i.e. `r_upscaler dlss` in the REPL also works).
void install_gx_cvars(lua_State* L);

// Called from Vm::shutdown() to deregister the on_change callbacks,
// preventing them from firing against a dead lua_State after the VM exits.
// Safe to call if install_gx_cvars was never called (no-op check inside).
void uninstall_gx_cvars();

}  // namespace psynder::script::detail
