// SPDX-License-Identifier: MIT
// Psynder — deferred structural-change command buffer (ADR-018 "effect bucket").
//
// Systems must never mutate ECS structure (spawn / destroy / add / remove a
// component) while iterating: it invalidates chunk iteration, breaks cache
// coherence, and — across worker threads — is nondeterministic. Instead a
// system *records* the change into a CommandBuffer and the buffer is replayed
// at a sync point (between system phases), deterministically and in one batch.
//
//   world.for_each_chunk_with_entities<Health>(
//       [&](usize n, const Entity* e, Health* h) {
//           for (usize i = 0; i < n; ++i)
//               if (h[i].hp <= 0) cb.destroy(e[i]);   // deferred
//       });
//   cb.playback(world);   // applied here, safely, then the buffer resets
//
// Spawning returns a *temp* handle so a freshly-spawned entity can be populated
// before it exists; playback resolves temp handles to the real entities it
// creates, in record order.
//
// Performance / determinism contract:
//   * Pooled: reserve() once; record/clear reuse the same storage — no
//     per-frame heap allocation in steady state.
//   * Deterministic: playback applies commands in record order. (Multi-threaded
//     recording later merges per-worker buffers in a fixed worker order, so the
//     guarantee holds; record order within a buffer is already stable.)
//   * Type-safe: component application goes through the public World::add/remove
//     via a captureless trampoline — no raw-mutation backdoor into the ECS.

#pragma once

#include "core/Types.h"
#include "scene/World.h"

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

namespace psynder::scene {

class CommandBuffer {
public:
    CommandBuffer() = default;

    // Pre-size the pools once at setup so steady-state frames never allocate.
    void reserve(usize commands, usize payload_bytes);

    // ─── Recording (call during iteration; nothing is applied yet) ────────
    // Spawn an entity at playback. Returns a temp handle usable immediately as
    // the target of add()/destroy() in this same buffer.
    [[nodiscard]] Entity create();

    // Destroy a real or temp entity at playback.
    void destroy(Entity e);

    // Add (a copy of) a component to a real or temp entity at playback.
    template <class T>
    void add(Entity e, const T& component);

    // Remove a component from a real or temp entity at playback.
    template <class T>
    void remove(Entity e);

    // ─── Replay ───────────────────────────────────────────────────────────
    // Apply every recorded command, in record order, then clear() the buffer.
    void playback(World& world);

    // Drop all recorded commands but keep the pooled capacity.
    void clear() noexcept;

    [[nodiscard]] usize size()  const noexcept { return commands_.size(); }
    [[nodiscard]] bool  empty() const noexcept { return commands_.empty(); }

    // Pooled-capacity high-water marks (diagnostics / pooling assertions).
    [[nodiscard]] usize command_capacity() const noexcept { return commands_.capacity(); }
    [[nodiscard]] usize payload_capacity() const noexcept { return payload_.capacity(); }

private:
    enum class Op : u8 { Create, Destroy, Add, Remove };

    struct Command {
        Op       op;
        Entity   entity;          // target (temp or real); ignored for Create
        u32      payload_offset;  // into payload_ (Add only)
        u32      payload_size;    // (Add only)
        void   (*apply)(World&, Entity, const void*);  // Add/Remove trampoline
    };

    // Top bit of the 64-bit handle tags a temp entity; the rest is its index
    // into the create-order table built during playback. Real generations
    // start at 1 and increment, so this bit is never set on a real handle in
    // any practical match.
    static constexpr u64 kTempBit = u64{1} << 63;

    u32    push_payload(const void* src, usize size, usize align);
    Entity resolve(Entity e) const noexcept;

    std::vector<Command>   commands_;
    std::vector<std::byte> payload_;
    std::vector<Entity>    temp_to_real_;  // temp index -> created entity
    u64                    temp_count_ = 0;
};

template <class T>
void CommandBuffer::add(Entity e, const T& component) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    const u32 offset = push_payload(&component, sizeof(T), alignof(T));
    commands_.push_back(Command{
        Op::Add, e, offset, static_cast<u32>(sizeof(T)),
        [](World& world, Entity ent, const void* payload) {
            // Copy out of the byte arena into a properly-aligned T before use.
            // The arena's base pointer is not guaranteed aligned for over-aligned
            // components (e.g. alignas(64)), so dereferencing payload as T* would
            // be UB; memcpy has no alignment requirement.
            T value;
            std::memcpy(&value, payload, sizeof(T));
            world.add<T>(ent, value);
        }});
}

template <class T>
void CommandBuffer::remove(Entity e) {
    commands_.push_back(Command{
        Op::Remove, e, 0u, 0u,
        [](World& world, Entity ent, const void*) { world.remove<T>(ent); }});
}

}  // namespace psynder::scene
