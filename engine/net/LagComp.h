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

#include "TickConfig.h"
#include "core/Types.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────────
// WorldState — opaque to the net layer. Game/physics defines the real type;
// until M6 / lane 17 settles the contract, we carry it as a void pointer
// paired with a size. Real hitreg code will cast to a concrete type.
// ──────────────────────────────────────────────────────────────────────────────
struct OpaqueWorldState {
    void*  data     = nullptr;
    usize  size     = 0;
};

// ──────────────────────────────────────────────────────────────────────────────
// RewindResult — returned by rewind_world_to() to communicate the quality of
// the rewind. Callers should reject a shot if the rewind was clamped or
// unavailable (e.g. server just started, no history yet).
// ──────────────────────────────────────────────────────────────────────────────
enum class RewindResult : u8 {
    Ok         = 0,  // Exact or interpolated rewind; safe to validate hit.
    Clamped    = 1,  // Requested time older than the rewind buffer; state at
                     // buffer head returned instead. Fire with caution.
    Unavailable = 2, // No history in buffer at all; caller should reject hit.
};

// ──────────────────────────────────────────────────────────────────────────────
// LagCompContext — per-server state for the rolling world history buffer.
//
// Wave B stub: the context is opaque; M6 will define its internals. For now
// all operations are no-ops that always report Unavailable.
// ──────────────────────────────────────────────────────────────────────────────
class LagCompContext {
public:
    explicit LagCompContext(const TickConfig& cfg) noexcept : cfg_(cfg) {}

    // Returns the depth of the rewind buffer in ticks (= cfg_.lag_comp_ticks).
    u32 max_rewind_ticks() const noexcept { return cfg_.lag_comp_ticks; }

    const TickConfig& tick_config() const noexcept { return cfg_; }

private:
    TickConfig cfg_;
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
// STUB: Wave B returns Unavailable and leaves out_state unchanged.
// ──────────────────────────────────────────────────────────────────────────────
inline RewindResult rewind_world_to(
    LagCompContext&   ctx,
    OpaqueWorldState& out_state,
    f64               client_view_time_ms,
    u32               current_tick,
    u16               sub_tick_frac_u16) noexcept
{
    // Suppress unused-parameter warnings for stub.
    (void)ctx;
    (void)out_state;
    (void)client_view_time_ms;
    (void)current_tick;
    (void)sub_tick_frac_u16;

    // M6 will replace this with:
    //   1. Convert client_view_time_ms to a tick + sub-tick fraction.
    //   2. Clamp to the rewind window (ctx.max_rewind_ticks()).
    //   3. Fetch the rolling snapshot for that tick from the ring buffer.
    //   4. If sub_tick_frac_u16 != 0: linearly interpolate between tick T and
    //      T+1 snapshots, then write into out_state.
    //   5. Return Ok or Clamped based on whether clamping occurred.
    return RewindResult::Unavailable;
}

// ──────────────────────────────────────────────────────────────────────────────
// push_world_snapshot — record the current world state into the lag-comp
// buffer. Called once per server tick, before handing control to game code.
//
// STUB: no-op in Wave B.
// ──────────────────────────────────────────────────────────────────────────────
inline void push_world_snapshot(
    LagCompContext&       ctx,
    const OpaqueWorldState& state,
    u32                   tick) noexcept
{
    (void)ctx;
    (void)state;
    (void)tick;
    // M6: append state to the ring buffer indexed by `tick`.
    // Evict entries older than ctx.max_rewind_ticks().
}

}  // namespace psynder::net
