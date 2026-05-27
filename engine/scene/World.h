// SPDX-License-Identifier: MIT
// Psynder — archetype-chunked ECS world. Lane 06 owns the full ECS impl
// (chunks, archetypes, queries, structural batching, spatial indices).
//
// This header freezes the contract used by every other subsystem.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace psynder::scene {

// ─── Component registration ──────────────────────────────────────────────
// Components are POD structs. Use PSYNDER_COMPONENT(Name) to declare; the
// macro asserts triviality at compile time and registers the type at static
// init. The lane 06 implementation provides the registry; this header makes
// the macro available everywhere.

using ComponentId = u32;

struct ComponentTypeInfo {
    ComponentId  id;
    u32          size;
    u32          align;
    const char*  name;
};

ComponentId register_component(const ComponentTypeInfo& info);

template <class T>
ComponentId component_id() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    static const ComponentId id = register_component(
        ComponentTypeInfo{ 0, sizeof(T), alignof(T), "" });
    return id;
}

#define PSYNDER_COMPONENT(Name) \
    struct Name; \
    /* clang-format off */ \
    inline auto Psynder_Register_##Name = ::psynder::scene::component_id<Name>(); \
    /* clang-format on */ \
    struct Name

// ─── Chunk view ───────────────────────────────────────────────────────────
// One archetype chunk's contiguous columns, handed to a system per chunk.
// `columns[i]` is the base of the i-th requested component's array (cast to
// T*). Pointers are into live chunk memory and valid only for the duration of
// the callback that received the view.
struct ChunkView {
    usize         count    = 0;
    const Entity* entities = nullptr;  // the chunk's entity-id column
    void* const*  columns  = nullptr;  // one base pointer per requested id
};

// ─── World ───────────────────────────────────────────────────────────────
// Per-instance ECS storage. The entity table, archetypes and chunks live in
// each World (pimpl below), NOT in a process-global — so a server can create
// and tear down a match world, replay can spin up a clean world to rewind
// into, and tests get isolation. Component *type* registration stays global
// (it is program-wide reflection, like type ids). Get() returns the default
// world for code that just wants "the" world.
struct WorldState;

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    static World& Get();

    Entity create();
    void   destroy(Entity e);
    bool   alive(Entity e) const noexcept;

    template <class T>
    void add(Entity e, const T& component);

    template <class T>
    T* get(Entity e);

    template <class T>
    void remove(Entity e);

    // ─── Chunk query / iteration (the system hot path) ────────────────────
    // Visit every archetype chunk that contains all of Ts..., calling
    //   fn(usize count, Ts*... columns)
    // once per chunk. `columns` are the chunk's contiguous component arrays
    // (component-major: T[0]..T[count-1]) — raw pointers into live chunk
    // memory, valid only for that call. There is no per-entity indirection,
    // no virtual dispatch and no allocation: a system body is a tight loop
    // over count, which the compiler auto-vectorises for simple field updates.
    // Iterate, then write `for (usize i = 0; i < count; ++i) col[i].field = …`.
    template <class... Ts, class Fn>
    void for_each_chunk(Fn&& fn);

    // As for_each_chunk, but also passes the chunk's entity-id column:
    //   fn(usize count, const Entity* entities, Ts*... columns)
    // Use when a system needs entity ids (e.g. to enqueue deferred effects).
    template <class... Ts, class Fn>
    void for_each_chunk_with_entities(Fn&& fn);

    // Low-level type-erased iteration primitive backing the typed wrappers.
    // Visits each chunk whose archetype contains all `ids` (archetype/chunk
    // order), invoking fn(ctx, view) once per non-empty chunk. Prefer the
    // typed wrappers; this exists for codegen / non-template callers.
    void query_chunks(const ComponentId* ids, usize id_count,
                      void* ctx, void (*fn)(void*, const ChunkView&));

private:
    void  add_component(Entity e, const ComponentTypeInfo& info, const void* component);
    void* get_component(Entity e, ComponentId id) noexcept;
    void  remove_component(Entity e, ComponentId id);

    std::unique_ptr<WorldState> state_;
};

// ─── Tag traits for read/write declarations ──────────────────────────────
template <class... Ts> struct reads  {};
template <class... Ts> struct writes {};

template <class T>
void World::add(Entity e, const T& component) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    const ComponentId id = component_id<T>();
    add_component(e, ComponentTypeInfo{ id, sizeof(T), alignof(T), "" }, &component);
}

template <class T>
T* World::get(Entity e) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    return static_cast<T*>(get_component(e, component_id<T>()));
}

template <class T>
void World::remove(Entity e) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    remove_component(e, component_id<T>());
}

// ─── Chunk-iteration template glue ────────────────────────────────────────
namespace detail {
template <class F, class... Ts, usize... I>
inline void invoke_chunk(F& fn, const ChunkView& v, std::index_sequence<I...>) {
    fn(v.count, static_cast<Ts*>(v.columns[I])...);
}
template <class F, class... Ts, usize... I>
inline void invoke_chunk_e(F& fn, const ChunkView& v, std::index_sequence<I...>) {
    fn(v.count, v.entities, static_cast<Ts*>(v.columns[I])...);
}
}  // namespace detail

template <class... Ts, class Fn>
void World::for_each_chunk(Fn&& fn) {
    static_assert(sizeof...(Ts) >= 1, "for_each_chunk needs at least one component");
    using F = std::remove_reference_t<Fn>;
    const ComponentId ids[sizeof...(Ts)] = { component_id<Ts>()... };
    query_chunks(ids, sizeof...(Ts), &fn, [](void* ctx, const ChunkView& v) {
        detail::invoke_chunk<F, Ts...>(*static_cast<F*>(ctx), v,
                                       std::index_sequence_for<Ts...>{});
    });
}

template <class... Ts, class Fn>
void World::for_each_chunk_with_entities(Fn&& fn) {
    static_assert(sizeof...(Ts) >= 1, "for_each_chunk_with_entities needs a component");
    using F = std::remove_reference_t<Fn>;
    const ComponentId ids[sizeof...(Ts)] = { component_id<Ts>()... };
    query_chunks(ids, sizeof...(Ts), &fn, [](void* ctx, const ChunkView& v) {
        detail::invoke_chunk_e<F, Ts...>(*static_cast<F*>(ctx), v,
                                         std::index_sequence_for<Ts...>{});
    });
}

}  // namespace psynder::scene
