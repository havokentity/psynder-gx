// SPDX-License-Identifier: MIT
// Psynder-GX script lane — GX-specific cvar binding impl.
//
// Registers r_upscaler, r_dynamic_res, cl_tickrate with lane 01's
// psynder::console::Console, then installs a `gx` Lua global with typed
// getter/setter functions so scripts can read/write these cvars without
// knowing the string-based console internals.

#include "GxCvars.h"

#include "core/Log.h"
#include "core/console/Console.h"

#include <algorithm>  // std::clamp
#include <atomic>
#include <string>

namespace psynder::script::detail {

namespace {

// ── CVar registration state ──────────────────────────────────────────────

// Pointers to registered cvars. Set once in install_gx_cvars(); cleared in
// uninstall_gx_cvars().  Both functions are called on the main/script thread.
// No locking needed — Console lifetime encompasses Vm lifetime.
::psynder::console::CVar* g_cvar_upscaler    = nullptr;
::psynder::console::CVar* g_cvar_dynamic_res = nullptr;
::psynder::console::CVar* g_cvar_tickrate    = nullptr;

// Flag to avoid double-registration if Vm is restarted in the same process.
std::atomic<bool> g_installed{false};

// ── Lua getters/setters ───────────────────────────────────────────────────

// gx.r_upscaler() -> string
int l_gx_get_r_upscaler(lua_State* L) {
    if (!g_cvar_upscaler) {
        lua_pushliteral(L, "off");
        return 1;
    }
    lua_pushstring(L, g_cvar_upscaler->value.c_str());
    return 1;
}

// gx.set_r_upscaler(val: string) -> nil
int l_gx_set_r_upscaler(lua_State* L) {
    const char* val = luaL_checkstring(L, 1);
    if (!g_cvar_upscaler) {
        return luaL_error(L, "gx: r_upscaler cvar not installed");
    }
    // Validate against the allowed set.
    static const char* const kAllowed[] = {
        "off", "dlss", "fsr", "xess", "metalfx", nullptr
    };
    bool valid = false;
    for (int i = 0; kAllowed[i]; ++i) {
        if (std::string_view(val) == kAllowed[i]) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        return luaL_error(L,
            "gx.set_r_upscaler: invalid value '%s' "
            "(allowed: off|dlss|fsr|xess|metalfx)", val);
    }
    ::psynder::console::Console::Get().SetCVarOverride("r_upscaler", val);
    return 0;
}

// gx.r_dynamic_res() -> number  [0.5, 1.0]
int l_gx_get_r_dynamic_res(lua_State* L) {
    if (!g_cvar_dynamic_res) {
        lua_pushnumber(L, 1.0);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(g_cvar_dynamic_res->GetFloat()));
    return 1;
}

// gx.set_r_dynamic_res(val: number) -> nil
int l_gx_set_r_dynamic_res(lua_State* L) {
    if (!g_cvar_dynamic_res) {
        return luaL_error(L, "gx: r_dynamic_res cvar not installed");
    }
    lua_Number val = luaL_checknumber(L, 1);
    float clamped  = std::clamp(static_cast<float>(val), 0.5f, 1.0f);
    std::string s  = std::to_string(clamped);
    // Trim trailing zeros for readability (e.g. "0.750000" -> "0.75").
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last != std::string::npos && last > dot) {
            s.erase(last + 1);
        } else if (last == dot) {
            s.erase(dot);  // "1." -> "1"
        }
    }
    ::psynder::console::Console::Get().SetCVarOverride("r_dynamic_res", s);
    return 0;
}

// gx.cl_tickrate() -> integer  [20, 128]
int l_gx_get_cl_tickrate(lua_State* L) {
    if (!g_cvar_tickrate) {
        lua_pushinteger(L, 64);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(g_cvar_tickrate->GetInt()));
    return 1;
}

// gx.set_cl_tickrate(val: integer) -> nil
int l_gx_set_cl_tickrate(lua_State* L) {
    if (!g_cvar_tickrate) {
        return luaL_error(L, "gx: cl_tickrate cvar not installed");
    }
    lua_Integer val     = luaL_checkinteger(L, 1);
    int clamped         = static_cast<int>(
        std::clamp(static_cast<int>(val), 20, 128));
    ::psynder::console::Console::Get().SetCVarOverride(
        "cl_tickrate", std::to_string(clamped));
    return 0;
}

const luaL_Reg kGxMethods[] = {
    { "r_upscaler",       l_gx_get_r_upscaler    },
    { "set_r_upscaler",   l_gx_set_r_upscaler    },
    { "r_dynamic_res",    l_gx_get_r_dynamic_res },
    { "set_r_dynamic_res",l_gx_set_r_dynamic_res },
    { "cl_tickrate",      l_gx_get_cl_tickrate   },
    { "set_cl_tickrate",  l_gx_set_cl_tickrate   },
    { nullptr, nullptr }
};

}  // namespace

void install_gx_cvars(lua_State* L) {
    if (g_installed.exchange(true)) {
        // Already registered (e.g. Vm restarted without a clean shutdown).
        // Re-install the Lua table so the new lua_State gets the bindings,
        // but skip Console registration to avoid duplicate cvar entries.
        lua_newtable(L);
        luaL_setfuncs(L, kGxMethods, 0);
        lua_setglobal(L, "gx");
        return;
    }

    auto& con = ::psynder::console::Console::Get();

    // r_upscaler — which upscale backend the renderer should use.
    // ARCHIVE so the player's choice persists across sessions.
    g_cvar_upscaler = con.RegisterCVar(
        "r_upscaler", "off",
        "Upscale backend: off | dlss | fsr | xess | metalfx",
        ::psynder::console::CVAR_ARCHIVE,
        [](const ::psynder::console::CVar& cv) {
            PSY_LOG_INFO("script/gx: r_upscaler changed to '{}'", cv.value);
        });

    // r_dynamic_res — fractional internal resolution scale [0.5, 1.0].
    // 1.0 = native resolution, 0.5 = half resolution (combined with upscaler).
    g_cvar_dynamic_res = con.RegisterCVar(
        "r_dynamic_res", "1.0",
        "Dynamic resolution scale [0.5, 1.0]; 1.0 = native",
        ::psynder::console::CVAR_ARCHIVE,
        [](const ::psynder::console::CVar& cv) {
            PSY_LOG_INFO("script/gx: r_dynamic_res changed to '{}'", cv.value);
        });

    // cl_tickrate — client simulation tick frequency (Hz) [20, 128].
    // 64 is a sane default matching Psynder's default tickrate.
    // NOTE: the network layer (lane 18) reads this cvar by name; it does not
    // take a direct pointer. Thread safety of the read is lane 18's concern.
    g_cvar_tickrate = con.RegisterCVar(
        "cl_tickrate", "64",
        "Client simulation tickrate in Hz [20, 128]",
        ::psynder::console::CVAR_NONE,
        [](const ::psynder::console::CVar& cv) {
            PSY_LOG_INFO("script/gx: cl_tickrate changed to '{}'", cv.value);
        });

    // Install the `gx` table on the Lua state so scripts can call e.g.
    //   gx.set_r_upscaler("fsr")
    //   print(gx.cl_tickrate())
    lua_newtable(L);
    luaL_setfuncs(L, kGxMethods, 0);
    lua_setglobal(L, "gx");

    PSY_LOG_INFO("script/gx: GX cvars installed "
                 "(r_upscaler, r_dynamic_res, cl_tickrate)");
}

void uninstall_gx_cvars() {
    if (!g_installed.load()) {
        return;  // Never installed; safe no-op.
    }
    // Nullify the on_change callbacks so they cannot fire against a dead
    // lua_State after Vm::shutdown(). Console does not provide a deregister
    // API for cvars (by design — cvars live for the process); we just clear
    // our side of the callback.
    //
    // UNTESTED PATH (2026-05-19): this nullification races with Console::Drain()
    // if Drain() fires an on_change mid-shutdown. The Console mutex protects
    // the cvar map, but on_change is called outside the lock with a copy of
    // the CVar struct. If the std::function captures anything from the Lua VM
    // and Drain() runs concurrently with Vm::shutdown(), we have a use-after-
    // free. Current mitigations: (a) on_change only calls PSY_LOG_INFO — no
    // lua_State access — so the UAF doesn't manifest. (b) The engine is
    // expected to stop Drain() before Vm::shutdown(). Neither guarantee is
    // tested.
    if (g_cvar_upscaler)    { g_cvar_upscaler->on_change    = nullptr; }
    if (g_cvar_dynamic_res) { g_cvar_dynamic_res->on_change = nullptr; }
    if (g_cvar_tickrate)    { g_cvar_tickrate->on_change    = nullptr; }

    g_installed.store(false);
}

}  // namespace psynder::script::detail
