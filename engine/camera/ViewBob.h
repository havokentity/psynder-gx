// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/ViewBob.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe walk-cycle
// HEAD-BOB for the view. The locomotion analogue of Recoil's per-shot kick and
// Shake's trauma jolt (see Recoil.h / Shake.h): where recoil is a directed
// spray climb and shake is an omnidirectional impact, the bob is the gentle,
// rhythmic sway of the camera as the pawn WALKS — the classic FPS "footstep"
// bounce.
//
// It produces a small ADDITIVE POSITIONAL offset (in METRES) plus a slight
// ROLL (in DEGREES, in the same conventions as CameraState — see Camera.h) that
// a caller folds onto the view each frame while the pawn is moving:
//
//     CameraState cam = ...;                 // integrated look + position
//     BobState    b   = ...;                 // per-pawn walk-phase accumulator
//
//     // ... each frame, by how far the pawn moved this tick (planar speed*dt):
//     bob_advance(b, distance_travelled_m, params);
//     const BobOffset o = bob_sample(b, params);
//     cam.position[0] += o.pos[0];           // lateral sway (camera-right)
//     cam.position[1] += o.pos[1];           // vertical bounce
//     // o.pos[2] is always 0; o.roll_deg is folded in by the view builder
//
//     // ... when the pawn stops / lands:
//     bob_reset(b);                          // freeze the bob at rest
//
// The phase advances with DISTANCE TRAVELLED, not with wall-clock time: the bob
// speeds up as the pawn moves faster and FREEZES the instant the pawn stops
// (zero distance => zero phase advance). One full bob cycle spans `cycle_len_m`
// metres of travel. The caller maps the additive `pos[0]` (lateral) onto the
// camera's RIGHT axis and `pos[1]` (vertical) onto world-up when folding the
// offset in; the bob itself works in the camera-local sway/bounce frame.
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - BobState / BobParams are PODs owned by the CALLER. No globals, no
//     singletons, no hidden statics. The API is a pure data transform on the
//     caller-owned BobState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a BobState
//     can be snapshotted into a replay / rollback buffer with a plain memcpy.
//   - One BobState per pawn; N states drive trivially in parallel from N
//     JobSystem workers (no shared mutable state).
//
// Determinism: UNLIKE Recoil / Shake (which avoid libm entirely), the bob is a
// pure walk-cycle waveform and the textbook sin()-based formulation reads best,
// so it DOES use std::sin. That is acceptable here because the bob is purely
// COSMETIC — it is folded onto the VIEW, never into the authoritative lockstep
// simulation tick, so a sub-ulp libm divergence can never desync gameplay.
// macOS libm / glibc / MSVCRT disagree on sin at sub-ulp precision (Camera.h's
// note / Copilot PR #13), so cross-platform bit-identity is NOT claimed. The TU
// is compiled strict-FP (`-fno-fast-math -ffp-contract=off` / `/fp:strict`) so
// the same (state, params) yields a bit-identical offset across runs of the
// SAME platform — the lane's standard determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Per-pawn walk-phase accumulator (POD) ───────────────────────────────
// `phase` is the accumulated walk-cycle position in RADIANS, kept wrapped into
// [0, 2*pi) for numeric stability (so it never grows unbounded over a long
// match and lose float precision). Caller owns the storage. Trivially-copyable
// for snapshot / rollback.
struct BobState {
    f32 phase = 0.0f;   // accumulated walk phase, radians, wrapped [0, 2*pi)
};

// Layout assertion — a single f32 = 4 bytes, 4-byte aligned.
static_assert(sizeof(BobState) == 4, "BobState POD layout drifted");
static_assert(alignof(BobState) <= 4, "BobState should not over-align");

// ─── Walk-bob tuning (POD) ───────────────────────────────────────────────
// Authored per locomotion-class (offline / data-driven). Pure tuning — no
// runtime state lives here, so a single BobParams can be shared (by const&) by
// every pawn using that gait. The amp fields are the PEAK displacement of each
// channel; `cycle_len_m` is how far the pawn must travel for one full bob
// cycle (one lateral sway = one full stride pair).
struct BobParams {
    f32 vertical_amp_m;   // peak vertical bounce (+/-), metres
    f32 lateral_amp_m;    // peak lateral sway   (+/-), metres
    f32 roll_amp_deg;     // peak view roll      (+/-), degrees
    f32 cycle_len_m;      // travel distance of one full bob cycle, metres
};

// ─── This-frame additive view offset (POD) ───────────────────────────────
// The bob offset to fold onto the camera this frame. Position is METRES
// (pos[0] = lateral sway along camera-right, pos[1] = vertical bounce along
// world-up, pos[2] is always 0); roll is DEGREES.
struct BobOffset {
    f32 pos[3] = {0.0f, 0.0f, 0.0f};   // x = lateral, y = vertical, z = 0
    f32 roll_deg = 0.0f;
};

// ─── Bob operations (stateless aside from the BobState) ──────────────────
// Advance the walk phase by the distance the pawn travelled this tick. The
// phase grows by `2*pi * distance_travelled_m / cycle_len_m`, then is wrapped
// back into [0, 2*pi). A stopped pawn (distance_travelled_m == 0) does NOT
// advance the phase, so the bob freezes in place rather than drifting. A
// non-positive `cycle_len_m` (or a non-finite distance) is guarded: the phase
// is left UNCHANGED — no division by zero, no NaN/Inf leaking into the state.
void bob_advance(BobState& s, f32 distance_travelled_m, const BobParams& p) noexcept;

// Sample the additive view offset for the CURRENT phase. The waveform is the
// classic FIGURE-8 head motion:
//   - vertical = vertical_amp_m * sin(2 * phase)  — DOUBLE frequency: the head
//     dips once per FOOT-fall, i.e. TWICE per full lateral cycle (one stride
//     pair). Two bounces per one sway is what makes the head trace a figure-8.
//   - lateral  = lateral_amp_m  * sin(phase)      — single frequency: the body
//     sways once to each side over a full cycle.
//   - roll_deg = roll_amp_deg   * sin(phase)      — single frequency, in phase
//     with the lateral sway (the view tips toward the planted foot).
// pos[2] is always 0. Every channel is bounded by its amplitude (|sin| <= 1).
// Pure algebra + sin: same (state, params) => bit-identical offset (same
// platform — see the header's determinism note).
BobOffset bob_sample(const BobState& s, const BobParams& p) noexcept;

// Zero the walk phase (e.g. when the pawn stops moving or lands from a jump),
// so the bob settles back to its neutral rest pose for the next stride.
void bob_reset(BobState& s) noexcept;

}  // namespace psynder::camera
