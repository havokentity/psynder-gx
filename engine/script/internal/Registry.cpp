// SPDX-License-Identifier: MIT
// Psynder — script-lane registry impl.

#include "Registry.h"
#include <utility>

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lauxlib.h"
}
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

namespace psynder::script::detail {

namespace {
// Script-lane component ids set the top bit so they live in their own half of
// the id space (0x80000000..0xFFFFFFFF), visibly distinct from engine-side ids.
// Script ids never enter scene::World's component registry (engine-backed
// components use the compile-time component_id<T>()); this id is only a key for
// the script lane's own Lua-side component tables, so the two spaces never
// actually share a lookup. When lane 06 lands a real "find by name" shim this
// can collapse to a single id space.
inline constexpr scene::ComponentId kScriptIdBit = 0x80000000u;

// Stable id for a component NAME: the low 31 bits of the engine's FNV-1a-32
// (scene::fnv1a32) with the script bit forced on. The id is a pure function of
// the name — independent of registration / call order — so it is
// bit-reproducible across runs, builds, and peers (the lockstep determinism
// pillar; previously ids were minted by call order). Collision risk between two
// distinct names is ~1/2^31, the same hash-trust the engine's component_id<T>()
// already accepts.
scene::ComponentId id_for_name(const std::string& name) noexcept {
    return kScriptIdBit | (scene::fnv1a32(name.c_str()) & 0x7FFFFFFFu);
}
}  // namespace

scene::ComponentId ScriptRegistry::register_or_get(std::string_view name) {
    std::string key{name};
    auto it = names_.find(key);
    if (it != names_.end()) {
        return it->second;
    }
    const scene::ComponentId id = id_for_name(key);
    names_.emplace(key, id);
    ids_to_name_.emplace(id, std::move(key));
    return id;
}

scene::ComponentId ScriptRegistry::find(std::string_view name) const {
    auto it = names_.find(std::string{name});
    return it == names_.end() ? 0u : it->second;
}

std::string_view ScriptRegistry::name_of(scene::ComponentId id) const {
    auto it = ids_to_name_.find(id);
    return it == ids_to_name_.end() ? std::string_view{} : std::string_view{it->second};
}

usize ScriptRegistry::add_system(LuaSystem s) {
    systems_.push_back(std::move(s));
    return systems_.size() - 1;
}

std::span<LuaSystem> ScriptRegistry::systems() noexcept {
    return std::span<LuaSystem>(systems_);
}

std::span<const LuaSystem> ScriptRegistry::systems() const noexcept {
    return std::span<const LuaSystem>(systems_);
}

void ScriptRegistry::release_refs(lua_State* L) {
    if (!L) {
        systems_.clear();
        return;
    }
    for (auto& s : systems_) {
        if (s.fn_ref != LUA_NOREF && s.fn_ref != LUA_REFNIL) {
            luaL_unref(L, LUA_REGISTRYINDEX, s.fn_ref);
            s.fn_ref = LUA_NOREF;
        }
    }
    systems_.clear();
}

}  // namespace psynder::script::detail
