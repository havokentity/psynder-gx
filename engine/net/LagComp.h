// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lag compensation rewind API. Lane 18 (net).
//
// DESIGN-PSYNDER-GX.md §10.4 (Lag compensation):
//   "Server rewinds world state to the client's view-time when validating
//    hitscans. Standard rewind window: 200 ms."
//
// This header freezes the public API that game code (hitreg) will call.
// The implementation is a stub in Wave B; a full rolling-buffer world rewind
// lives in M6. Lane 17 (physics / destruction) owns the world snapshot format
// that this system consumes — see Issue #lag-comp-world-snapshot-api when that
// lane defines its WorldState type.
//
// Sub-tick precision note (DESIGN §10.4):
//   When a player fires, the client sends a sub-tick aim fraction alongside
//   the hit packet. `rewind_world_to` accepts that fraction as
//   `sub_tick_frac_u16` (0 = start of tick, 0xFFFF = end of tick). The server
//   linearly interpolates the rewound aim ray inside the identified tick.
//   This matches CS2-style hitreg fidelity ("the server uses this fraction
//   during lag compensation to construct the exact aim ray, not a
//   tick-quantized one").
//
// Compile flag: this TU MUST be compiled with
//   -fno-fast-math -ffp-contract=off
// per DESIGN-PSYNDER-GX.md §14.

#pragma once

#include <vector>

#include "TickConfig.h"
#include "core/Types.h"
#include "physics/destruction/PublicDestruction.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────────
// DestructionWorldState — typed lane-17 world-state bytes.
//
// Lane 17 owns the concrete wire layout in
// physics/destruction/PublicDestruction.h. The net layer still stores a deep
// copy of the bytes, but call sites now name the real producer contract instead
// of passing an anonymous void pointer payload.
// ──────────────────────────────────────────────────────────────────────────────
using DestructionWorldState = ::psynder::physics::destruction::WorldState;

// Compatibility for older call sites. New code should use
// DestructionWorldState so the lane-17 ownership is visible in the type name.
using OpaqueWorldState [[deprecated("use DestructionWorldState")]] = DestructionWorldState;

// ──────────────────────────────────────────────────────────────────────────────
// RewindResult — returned by rewind_world_to() to communicate the quality of
// the rewind. Callers should reject a shot if the rewind was clamped or
// unavailable (e.g. server just started, no history yet).
// ──────────────────────────────────────────────────────────────────────────────
enum class RewindResult : u8 {
    Ok = 0,           // Exact or interpolated rewind; safe to validate hit.
    Clamped = 1,      // Requested time older than the rewind buffer; state at
                      // buffer head returned instead. Fire with caution.
    Unavailable = 2,  // No history in buffer at all; caller should reject hit.
};

// ──────────────────────────────────────────────────────────────────────────────
// LagCompContext — per-server state for the rolling world history buffer.
//
// Carries a ring buffer of past world snapshots covering `cfg.lag_comp_ticks`
// ticks (= ceil(200ms / frame_ms) per DESIGN §10.4). Each stored snapshot is
// a deep copy of the bytes passed to `push_world_snapshot`. Older entries
// are evicted automatically as new ticks arrive.
//
// The WorldState payload is owned by lane 17 (physics-destruction). The buffer
// stores `(tick, bytes)` pairs and returns the same typed byte contract on
// rewind.
// ──────────────────────────────────────────────────────────────────────────────
class LagCompContext {
   public:
    explicit LagCompContext(const TickConfig& cfg) noexcept;
    ~LagCompContext() noexcept;

    LagCompContext(const LagCompContext&) = delete;
    LagCompContext& operator=(const LagCompContext&) = delete;

    // Returns the depth of the rewind buffer in ticks (= cfg_.lag_comp_ticks).
    u32 max_rewind_ticks() const noexcept { return cfg_.lag_comp_ticks; }

    const TickConfig& tick_config() const noexcept { return cfg_; }

    // Number of snapshots currently stored (0 .. max_rewind_ticks()).
    u32 stored_count() const noexcept;

    // Tick number of the oldest / newest stored snapshot. Returns 0 if
    // empty.
    u32 oldest_tick() const noexcept;
    u32 newest_tick() const noexcept;

    // Internal: append a snapshot. Public so the free function
    // push_world_snapshot() can delegate; not part of the documented API.
    void push_internal_(u32 tick, const DestructionWorldState& state) noexcept;

    // Internal: locate the snapshot bracketing `target_tick` (older + newer)
    // for linear interpolation. Returns false if buffer is empty.
    bool bracket_internal_(u32 target_tick,
                           const u8*& out_older_bytes,
                           u32& out_older_tick,
                           const u8*& out_newer_bytes,
                           u32& out_newer_tick,
                           usize& out_state_size) const noexcept;

   private:
    struct Slot {
        u32 tick = 0;
        std::vector<u8> bytes;
    };

    TickConfig cfg_;
    std::vector<Slot> ring_;
    u32 head_ = 0;  // index of newest entry
    u32 count_ = 0;
    u32 capacity_ = 0;
};

// ──────────────────────────────────────────────────────────────────────────────
// rewind_world_to — API contract (Wave B stub).
//
// Parameters:
//   ctx               — per-server lag-comp context (carries tick config +
//                       rolling snapshot buffer).
//   out_state         — destination to fill with the rewound world state.
//   client_view_time_ms — the client's view time as reported in the shot
//                       packet, in milliseconds (server clock base). The
//                       server clamps this to [server_now - 200ms, server_now].
//   current_tick      — the server's current tick number (for clamping).
//   sub_tick_frac_u16 — 16-bit sub-tick fraction from the client's input
//                       snapshot (DESIGN §10.4 sub-tick aim). 0 = start of
//                       tick, 0xFFFF = just before next tick boundary.
//
// Returns RewindResult indicating whether the rewind was exact, clamped, or
// not available. Callers reject hits on Unavailable.
//
// `out_state.data` must point at a caller-owned buffer of `out_state.size`
// bytes; on Ok / Clamped the bytes are filled with the rewound (and
// optionally interpolated) world state. If the stored snapshot size doesn't
// match `out_state.size`, the call returns Unavailable.
// ──────────────────────────────────────────────────────────────────────────────
RewindResult rewind_world_to(LagCompContext& ctx,
                             DestructionWorldState& out_state,
                             f64 client_view_time_ms,
                             u32 current_tick,
                             u16 sub_tick_frac_u16) noexcept;

// ──────────────────────────────────────────────────────────────────────────────
// push_world_snapshot — record the current world state into the lag-comp
// buffer. Called once per server tick, before handing control to game code.
// The bytes pointed at by `state.data` are deep-copied; the caller is free
// to mutate the buffer immediately after this returns.
// ──────────────────────────────────────────────────────────────────────────────
void push_world_snapshot(LagCompContext& ctx, const DestructionWorldState& state, u32 tick) noexcept;

}  // namespace psynder::net
