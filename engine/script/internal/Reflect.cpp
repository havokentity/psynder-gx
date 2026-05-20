// SPDX-License-Identifier: MIT
// Psynder-GX — script-lane reflection registry + the Lua `reflect` global.
//
// Two process-wide registries (components, systems) are populated at static
// init by the PSYNDER_SCRIPT_COMPONENT / PSYNDER_SCRIPT_SYSTEM macros (see
// script/Reflect.h). install_reflect_api() builds the read-only `reflect`
// table on a fresh lua_State so scripts and the developer REPL can introspect
// the engine's DOTS schema.

#include "script/Reflect.h"

#include "Bindings.h"  // install_reflect_api declaration (detail namespace)

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

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace psynder::script {

namespace {

// Stable-address storage. std::deque never relocates existing elements on
// push_back, so the pointers we hand out (and the spans into the name arrays)
// stay valid for the life of the process.
struct Registry {
    std::deque<ComponentReflection>                            components;
    std::vector<const ComponentReflection*>                    component_ptrs;
    std::unordered_map<std::string, const ComponentReflection*> component_by_name;

    std::deque<SystemReflection>                            systems;
    std::vector<const SystemReflection*>                    system_ptrs;
    std::unordered_map<std::string, const SystemReflection*> system_by_name;
    std::deque<std::vector<const char*>>                    name_arrays;
};

Registry& registry() {
    static Registry r;
    return r;
}

}  // namespace

const char* kind_name(FieldKind k) noexcept {
    switch (k) {
        case FieldKind::I8:      return "i8";
        case FieldKind::I16:     return "i16";
        case FieldKind::I32:     return "i32";
        case FieldKind::I64:     return "i64";
        case FieldKind::U8:      return "u8";
        case FieldKind::U16:     return "u16";
        case FieldKind::U32:     return "u32";
        case FieldKind::U64:     return "u64";
        case FieldKind::F32:     return "f32";
        case FieldKind::F64:     return "f64";
        case FieldKind::Bool:    return "bool";
        case FieldKind::Vec2:    return "vec2";
        case FieldKind::Vec3:    return "vec3";
        case FieldKind::Vec4:    return "vec4";
        case FieldKind::Quat:    return "quat";
        case FieldKind::Mat3:    return "mat3";
        case FieldKind::Mat4:    return "mat4";
        case FieldKind::IVec2:   return "ivec2";
        case FieldKind::IVec3:   return "ivec3";
        case FieldKind::Handle:  return "handle";
        case FieldKind::Unknown: break;
    }
    return "unknown";
}

const ComponentReflection* register_component_reflection(const char* name,
                                                         scene::ComponentId id,
                                                         u32              size,
                                                         u32              align,
                                                         const FieldDesc* fields,
                                                         u32 field_count) {
    Registry& r = registry();
    std::string key{name};
    if (auto it = r.component_by_name.find(key); it != r.component_by_name.end()) {
        return it->second;  // idempotent by name
    }
    r.components.push_back(ComponentReflection{
        name, id, size, align, std::span<const FieldDesc>(fields, field_count)});
    const ComponentReflection* p = &r.components.back();
    r.component_ptrs.push_back(p);
    r.component_by_name.emplace(std::move(key), p);
    return p;
}

const ComponentReflection* find_component_reflection(std::string_view name) noexcept {
    Registry& r = registry();
    auto it = r.component_by_name.find(std::string{name});
    return it == r.component_by_name.end() ? nullptr : it->second;
}

std::span<const ComponentReflection* const> all_component_reflections() noexcept {
    return registry().component_ptrs;
}

const SystemReflection* register_system_reflection(
    const char* name, SystemFn fn, std::initializer_list<const char*> reads,
    std::initializer_list<const char*> writes) {
    Registry& r = registry();
    std::string key{name};
    if (auto it = r.system_by_name.find(key); it != r.system_by_name.end()) {
        return it->second;  // idempotent by name
    }
    r.name_arrays.emplace_back(reads.begin(), reads.end());
    const std::vector<const char*>& reads_arr = r.name_arrays.back();
    r.name_arrays.emplace_back(writes.begin(), writes.end());
    const std::vector<const char*>& writes_arr = r.name_arrays.back();

    r.systems.push_back(SystemReflection{
        name, std::span<const char* const>(reads_arr.data(), reads_arr.size()),
        std::span<const char* const>(writes_arr.data(), writes_arr.size()), fn});
    const SystemReflection* p = &r.systems.back();
    r.system_ptrs.push_back(p);
    r.system_by_name.emplace(std::move(key), p);
    return p;
}

const SystemReflection* find_system_reflection(std::string_view name) noexcept {
    Registry& r = registry();
    auto it = r.system_by_name.find(std::string{name});
    return it == r.system_by_name.end() ? nullptr : it->second;
}

std::span<const SystemReflection* const> all_system_reflections() noexcept {
    return registry().system_ptrs;
}

namespace detail {

// Defined in GxReflect.cpp. Calling it (a) forces that translation unit to be
// linked out of the static lib and (b) guarantees its static-init component /
// system registrations have completed before we read the registries.
void anchor_gx_reflections() noexcept;

namespace {

// Push an array (1-based Lua sequence) of string names from a span.
void push_name_array(lua_State* L, std::span<const char* const> names) {
    lua_createtable(L, static_cast<int>(names.size()), 0);
    lua_Integer i = 1;
    for (const char* n : names) {
        lua_pushstring(L, n);
        lua_rawseti(L, -2, i);
        ++i;
    }
}

void push_component_table(lua_State* L, const ComponentReflection& c) {
    lua_createtable(L, 0, 5);
    lua_pushstring(L, c.name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, static_cast<lua_Integer>(c.id));
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, static_cast<lua_Integer>(c.size));
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, static_cast<lua_Integer>(c.align));
    lua_setfield(L, -2, "align");

    lua_createtable(L, static_cast<int>(c.fields.size()), 0);
    lua_Integer fi = 1;
    for (const FieldDesc& f : c.fields) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, f.name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, kind_name(f.kind));
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, static_cast<lua_Integer>(f.offset));
        lua_setfield(L, -2, "offset");
        lua_pushinteger(L, static_cast<lua_Integer>(f.size));
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, fi);
        ++fi;
    }
    lua_setfield(L, -2, "fields");
}

void push_system_table(lua_State* L, const SystemReflection& s) {
    lua_createtable(L, 0, 3);
    lua_pushstring(L, s.name);
    lua_setfield(L, -2, "name");
    push_name_array(L, s.reads);
    lua_setfield(L, -2, "reads");
    push_name_array(L, s.writes);
    lua_setfield(L, -2, "writes");
}

// reflect.components() -> { name, ... }
int l_reflect_components(lua_State* L) {
    auto comps = all_component_reflections();
    lua_createtable(L, static_cast<int>(comps.size()), 0);
    lua_Integer i = 1;
    for (const ComponentReflection* c : comps) {
        lua_pushstring(L, c->name);
        lua_rawseti(L, -2, i);
        ++i;
    }
    return 1;
}

// reflect.component(name) -> table | nil
int l_reflect_component(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const ComponentReflection* c = find_component_reflection(name);
    if (!c) {
        lua_pushnil(L);
        return 1;
    }
    push_component_table(L, *c);
    return 1;
}

// reflect.systems() -> { name, ... }
int l_reflect_systems(lua_State* L) {
    auto systems = all_system_reflections();
    lua_createtable(L, static_cast<int>(systems.size()), 0);
    lua_Integer i = 1;
    for (const SystemReflection* s : systems) {
        lua_pushstring(L, s->name);
        lua_rawseti(L, -2, i);
        ++i;
    }
    return 1;
}

// reflect.system(name) -> table | nil
int l_reflect_system(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const SystemReflection* s = find_system_reflection(name);
    if (!s) {
        lua_pushnil(L);
        return 1;
    }
    push_system_table(L, *s);
    return 1;
}

const luaL_Reg kReflectFns[] = {
    {"components", l_reflect_components},
    {"component", l_reflect_component},
    {"systems", l_reflect_systems},
    {"system", l_reflect_system},
    {nullptr, nullptr},
};

// __newindex handler for the read-only `reflect` proxy: rejects all writes.
int l_reflect_readonly(lua_State* L) {
    return luaL_error(L, "reflect is read-only");
}

}  // namespace

void install_reflect_api(lua_State* L) {
    // Force GX component/system registrations to be linked + initialised.
    anchor_gx_reflections();

    // Expose the function table through a locked read-only proxy: reads go via
    // __index, every write raises, and the metatable is hidden so a script
    // cannot peel the guard off (`reflect.components = nil` errors).
    lua_newtable(L);  // fns
    luaL_setfuncs(L, kReflectFns, 0);

    lua_newtable(L);  // proxy
    lua_newtable(L);  // metatable
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");  // mt.__index = fns
    lua_pushcfunction(L, l_reflect_readonly);
    lua_setfield(L, -2, "__newindex");  // mt.__newindex = reject
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");  // hide the metatable
    lua_setmetatable(L, -2);             // setmetatable(proxy, mt)

    lua_setglobal(L, "reflect");  // _G.reflect = proxy
    lua_pop(L, 1);                // drop fns
}

}  // namespace detail

}  // namespace psynder::script
