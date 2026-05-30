// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/ThreatMemory.h
//
// A decaying "last-seen-enemy" memory for combat bots. When a bot sees an enemy
// it records WHERE it saw it (record); the memory ages every tick (tick) and is
// FORGOTTEN once it has aged past a timeout — so after losing line of sight a
// bot can still walk to the stale last-known position to re-acquire, then give
// up when the trail goes cold. This is the classic "investigate last-known
// position" sense/memory layer above target selection (see TargetSelect.h).
//
// The ai lane links only core + math, so this is a pure container over plain
// structs (no ECS / gameplay dependency); the caller feeds enemy ids + world
// positions from whatever its own components are.
//
// Determinism (lockstep pillar): fixed-capacity slots, pure float algebra (no
// RNG, no trig, no sqrt), and fully specified tie-breaks. Eviction on a full
// memory drops the OLDEST entry (largest age_s); ties break to the LOWEST slot
// index, mirroring FlowField's lowest-index rule. freshest() returns the entry
// with the SMALLEST age (most recently seen); ties break to the LOWEST enemy_id.
// The same sequence of operations always yields the same state on every
// platform. Built -fno-fast-math; no per-call heap in the hot path.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

// One remembered sighting. `active` distinguishes a live memory from a free /
// forgotten slot; an inactive slot's other fields are stale and must be ignored.
struct ThreatEntry {
    u32        enemy_id;  // caller's stable id for the seen enemy
    math::Vec3 last_pos;  // world position where it was last seen
    f32        age_s;     // seconds since last seen (0 == just recorded)
    bool       active;    // false == free / forgotten slot
};

// Fixed-capacity, deterministic store of the most recent sighting per enemy.
class ThreatMemory {
public:
    // `capacity` slots, all inactive. A capacity of 0 is legal (records are
    // simply dropped) but typically you size this to the max enemies a bot
    // tracks at once.
    explicit ThreatMemory(u32 capacity);

    // Forget everything: deactivate every slot. O(capacity).
    void clear() noexcept;

    // Refresh or insert the memory for `enemy_id`: set last_pos = pos, age_s = 0,
    // active = true. If `enemy_id` already has a slot it is reused in place (no
    // duplicate). If it is new and the memory is full, EVICT the oldest entry
    // (largest age_s; ties -> lowest slot index) and take its slot. With
    // capacity 0 the record is dropped. O(capacity), no heap.
    void record(u32 enemy_id, math::Vec3 pos) noexcept;

    // Age every active entry by `dt_s`, then forget (deactivate) any whose age
    // has grown strictly greater than `forget_after_s`. O(capacity), no heap.
    void tick(f32 dt_s, f32 forget_after_s) noexcept;

    // The active entry for `enemy_id`, or nullptr if none is remembered. The
    // pointer is valid until the next mutating call.
    const ThreatEntry* find(u32 enemy_id) const noexcept;

    // The active entry with the SMALLEST age_s (most recently seen). Ties break
    // to the LOWEST enemy_id. Writes it to `out` and returns true; returns false
    // (leaving `out` untouched) when nothing is active.
    bool freshest(ThreatEntry& out) const noexcept;

    // How many slots are currently active. O(capacity).
    u32 active_count() const noexcept;

    u32 capacity() const noexcept { return static_cast<u32>(slots_.size()); }

private:
    std::vector<ThreatEntry> slots_;
};

}  // namespace psynder::ai
