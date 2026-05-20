// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — OpaqueWorldState producer (lane 17, world-state slice).
//
// Lane 18 (engine/net/LagComp.h) accepts opaque snapshot bytes via
//   struct OpaqueWorldState { void* data; usize size; };
//   push_world_snapshot(ctx, state, tick);
//   rewind_world_to(ctx, out_state, view_time_ms, current_tick, sub_tick);
// but has no way to interpolate two snapshots whose layout it does not own.
// This header is that contract: it defines the byte layout that lane 17 packs
// and that lane 18 lerps between when its rewind window straddles two ticks.
//
// DESIGN-PSYNDER-GX.md §10.4 ("server uses the sub-tick fraction during lag
// compensation to construct the exact aim ray, not a tick-quantized one")
// requires interpolation between adjacent snapshots — that interpolator is
// `interpolate_world_state` below. Wire format and field order are frozen by
// the static_assert at the bottom of this header; reordering fields breaks
// every demo file on disk.
//
// DOTS contract (DESIGN §3 + project memory):
//   * POD records only — no virtual, no RTTI, no exceptions.
//   * No `std::shared_ptr` anywhere in this lane.
//   * Real metric units. Positions are metres, velocities m/s, yaw radians.
//   * Lockstep determinism: this TU is compiled with
//     `-fno-fast-math -ffp-contract=off` (DESIGN §14).
//
// Threading: `interpolate_world_state` is per-entity-parallelisable. Entity
// counts > 64 are split across `psynder::jobs::JobSystem` chunks; below 64
// the loop runs serially because the job-system fixed cost dominates.

#pragma once

#include "core/Types.h"

#include <cstddef>
#include <span>
#include <type_traits>

namespace psynder::worldstate {

// ──────────────────────────────────────────────────────────────────────────
// Wire-format constants.
// ──────────────────────────────────────────────────────────────────────────

// `'WS01'` packed big-endian as an ASCII tag so it shows up readable in a hex
// dump. Lane 18 / demo readers can use this to sanity-check a buffer before
// trusting `entity_count`.
inline constexpr u32 kWorldStateMagic =
    (static_cast<u32>('W') << 24) |
    (static_cast<u32>('S') << 16) |
    (static_cast<u32>('0') <<  8) |
    (static_cast<u32>('1') <<  0);

// Layout version. Bump if `EntityRecord` field order changes. Readers refuse
// any other version rather than silently misinterpreting bytes.
inline constexpr u16 kWorldStateVersion = 1;

// Hard cap so a corrupted header cannot ask us to allocate a multi-GB span.
// Comfortably above the 64-player budget (which is ~64 player entities + a
// few hundred AI / pickups / projectiles in M2+).
inline constexpr u16 kMaxEntities = 4096;

// Job-system threshold. Below this entity count we run serially; above we
// dispatch to `JobSystem::parallel_for`.
inline constexpr usize kParallelEntityThreshold = 64;

// ──────────────────────────────────────────────────────────────────────────
// Header — fixed-size prefix that begins every world-state byte buffer.
// 16 bytes (multiple of 8) so the following EntityRecord array is naturally
// aligned at offset 16 for both 32- and 64-bit reads.
// ──────────────────────────────────────────────────────────────────────────
struct WorldStateHeader {
    u32 magic;          // == kWorldStateMagic
    u16 version;        // == kWorldStateVersion
    u16 entity_count;   // number of EntityRecord that follow
    u32 reserved0;      // padding for future flags; must be 0
    u32 reserved1;      // padding for future tick reference; must be 0
};

static_assert(std::is_trivially_copyable_v<WorldStateHeader>,
              "WorldStateHeader must be trivially copyable (POD-on-the-wire)");
static_assert(sizeof(WorldStateHeader) == 16,
              "WorldStateHeader is part of the wire format; must stay 16 bytes");

// ──────────────────────────────────────────────────────────────────────────
// EntityRecord — one snapshot row.
//   * Position is world-space metres (1 unit = 1 metre per project memory).
//   * Yaw is radians around the Y axis; pitch / roll are not in the snapshot
//     because lag-comp only cares about the hitbox capsule which is
//     yaw-aligned. (Aim ray is reconstructed separately from the shot packet.)
//   * Velocity is m/s in world space.
//   * `flags` is a free 32 bits for the game layer (alive bit, ducking bit,
//     etc.).  Bitfields don't lerp — the interpolator snaps to whichever
//     side of the bracket `t` is closer to, with exact ties (t == 0.5)
//     resolving to `a` for determinism.  See the `interpolate_world_state`
//     contract below for the precise rule.
// ──────────────────────────────────────────────────────────────────────────
struct EntityRecord {
    u32 entity_id;        //  0..4
    u32 archetype_hash;   //  4..8   — stable across a and b for the same entity
    f32 pos_x;            //  8..12  — world-space metres
    f32 pos_y;            // 12..16  — world-space metres (up)
    f32 pos_z;            // 16..20  — world-space metres
    f32 yaw_y;            // 20..24  — yaw around Y axis, radians
    f32 vel_x;            // 24..28  — m/s
    f32 vel_y;            // 28..32  — m/s
    f32 vel_z;            // 32..36  — m/s
    u32 flags;            // 36..40  — game-layer bitfield
};

// The task contract says "Stride is exactly sizeof(EntityRecord) == 32 bytes",
// but the contract also enumerates: u32 entity_id, u32 archetype_hash,
// 4×float transform (pos.xyz + yaw_y), 3×float velocity, u32 flags
//   = 4 + 4 + 16 + 12 + 4 = 40 bytes.
// 32 is mathematically impossible with that field list. We hold to the field
// list (interpolation correctness depends on it) and assert the true stride
// of 40 so misreading the wire format trips a build-break, not a runtime
// data-corruption.
static_assert(std::is_trivially_copyable_v<EntityRecord>,
              "EntityRecord must be trivially copyable (POD-on-the-wire)");
static_assert(std::is_standard_layout_v<EntityRecord>,
              "EntityRecord must be standard-layout for offsetof to be portable");
static_assert(sizeof(EntityRecord) == 40,
              "EntityRecord wire size is 40 bytes; any other size breaks every "
              ".psydem demo file on disk. Do not change without bumping "
              "kWorldStateVersion AND coordinating with lane 18 via Issue.");
static_assert(alignof(EntityRecord) == 4,
              "EntityRecord must be 4-byte aligned for unaligned-safe read on "
              "x86/ARM. Bigger alignment would force padding in the wire.");

// ──────────────────────────────────────────────────────────────────────────
// Size of a serialised buffer for `entity_count` entities. Pure function.
// ──────────────────────────────────────────────────────────────────────────
constexpr usize compute_world_state_bytes(usize entity_count) noexcept {
    return sizeof(WorldStateHeader) + entity_count * sizeof(EntityRecord);
}

// ──────────────────────────────────────────────────────────────────────────
// write_world_state — packs `header + records` into `out`.
//
// `out.size()` MUST equal `compute_world_state_bytes(in.size())`. Asserts
// (debug) and returns silently in release if the contract is violated; this
// is a never-fail producer call site — callers compute the size themselves.
// `in.size()` MUST be <= kMaxEntities; oversized writes are asserted.
// ──────────────────────────────────────────────────────────────────────────
void write_world_state(std::span<const EntityRecord> in,
                       std::span<u8>                 out) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// read_world_state_header — peek the header without copying records.
//
// Returns false (and leaves outputs unchanged) on:
//   * buffer shorter than sizeof(WorldStateHeader)
//   * magic mismatch
//   * version mismatch
//   * declared entity_count would overrun the buffer
//   * declared entity_count > kMaxEntities
// ──────────────────────────────────────────────────────────────────────────
bool read_world_state_header(std::span<const u8> bytes,
                             u16*                out_version,
                             u16*                out_entity_count) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// interpolate_world_state — linearly blends snapshots `a` and `b` at `t`.
//
// Contract:
//   * `a` and `b` must be valid world-state buffers with identical
//     entity_count, version, and magic.
//   * For every entity in `a`, the same `(entity_id, archetype_hash)` pair
//     MUST appear somewhere in `b`. Records may appear in different orders
//     between `a` and `b`; this function does an O(n) hash-keyed lookup per
//     entity (the 64-player budget makes O(n^2) tolerable; with n > 64 the
//     parallel_for chunks each pay O(n) per chunk).
//   * `out.size()` MUST equal both `a.size()` and `b.size()`.
//   * `t` is clamped to [0, 1] internally; callers may pass values outside
//     this range without corrupting `out`, but the result is the clamped lerp.
//
// On any contract violation returns false and `out` is left UNDISTURBED —
// the header write is deferred until after the validation pass succeeds,
// so a failing call doesn't poison the destination buffer.  Callers may
// retry with a different `b` / `t` without first re-clearing `out`.
//
// Lerp rules:
//   * pos.x / pos.y / pos.z, yaw_y, vel.x / vel.y / vel.z — linear lerp.
//   * entity_id and archetype_hash are copied from `a` (must match `b`).
//   * flags is copied from whichever side of the bracket `t` is closer to
//     (snap, not blend, because bitfields don't lerp).  At t == 0.5 we
//     take from `a` to keep the result deterministic (`(t <= 0.5f) ? a : b`).
//
// Threading: serial below `kParallelEntityThreshold`; otherwise dispatched
// through `psynder::jobs::JobSystem::parallel_for`. Each entity is independent
// so there is no synchronisation inside the loop body.
// ──────────────────────────────────────────────────────────────────────────
bool interpolate_world_state(std::span<const u8> a,
                             std::span<const u8> b,
                             f32                 t,
                             std::span<u8>       out) noexcept;

}  // namespace psynder::worldstate
