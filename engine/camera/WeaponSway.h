// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/WeaponSway.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe WEAPON-SWAY model
// for the VIEWMODEL: when the player turns, the held weapon does not snap with
// the camera — it LAGS behind the look and then catches up, drifting a small
// cosmetic offset OPPOSITE the look direction before easing back to centre. It
// is the viewmodel sibling of FovKick's dynamic FOV, Lean's peek slide, Recoil's
// per-shot kick, LookSmoothing's input filter and ViewBob's gait sway (see
// FovKick.h / Lean.h / Recoil.h / LookSmoothing.h / ViewBob.h): a small POD
// accumulator the caller ticks each frame and folds back into the viewmodel.
//
//     SwayState  ws = ...;                   // per-weapon sway accumulator
//     SwayParams wp = ...;                   // authored push + clamp + recenter
//
//     // ... once, on spawn / weapon-select:
//     sway_init(ws);                          // zero the offset
//
//     // ... every frame, with this frame's look deltas (degrees):
//     sway_update(ws, wp, look_yaw_delta_deg, look_pitch_delta_deg, dt_s);
//     // The caller folds the offset into the viewmodel's local transform, e.g.
//     // a small translate of the gun mesh on the screen-X / screen-Y plane:
//     viewmodel.local_offset_x += ws.offset_x;
//     viewmodel.local_offset_y += ws.offset_y;
//
// Purely cosmetic: nothing here feeds simulation, hit registration, weapon aim,
// or replay divergence — it only drives where the GUN MESH sits on screen. It
// does NOT itself move the viewmodel or read aim; it produces the additive
// offset and leaves the apply to the caller, exactly as Recoil produces an
// additive angle and Lean an additive slide the caller folds in.
//
// Lag direction (held consistent across the whole API):
//   - The offset is pushed OPPOSITE the look delta — the gun LAGS the camera.
//     A look to the RIGHT (look_yaw_delta_deg > 0) drives offset_x NEGATIVE (the
//     gun trails toward the left of screen as the view sweeps right); a look to
//     the LEFT drives offset_x positive. Likewise look_pitch_delta_deg > 0
//     drives offset_y negative and a downward pitch drives it positive. The push
//     target is `-look_delta * sway_scale`, clamped to +/- max_offset.
//   - With NO look input the target is 0, so the same ease that follows the push
//     also RECENTERS the gun: the offset relaxes back to rest when the look
//     stops. (The yaw/pitch sign that the caller feeds — CameraInput's
//     convention — only flips which screen side the gun trails to; the LAG, i.e.
//     opposite-sign, holds regardless.)
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - SwayState / SwayParams are PODs owned by the CALLER. No globals, no
//     singletons, no hidden statics. The API is a pure data transform on the
//     caller-owned SwayState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a SwayState
//     can be snapshotted into a replay / rollback buffer with a plain memcpy.
//   - One SwayState per weapon / viewmodel; N states drive trivially in parallel
//     from N JobSystem workers (no shared mutable state).
//
// Determinism: the model uses ONLY +, -, *, / and comparisons — NO trigonometry,
// NO floating-point RNG, NO wall-clock reads. macOS libm / glibc / MSVCRT
// disagree on sin/cos/tan at sub-ulp precision (Camera.h's note / Copilot
// PR #13), so the sway avoids them entirely: the same (state, params, inputs)
// yields a bit-identical result. The TU is compiled strict-FP (`-fno-fast-math
// -ffp-contract=off` / `/fp:strict`) so even the +,-,*,/ chain is reproducible
// across runs of the SAME platform, which is the lane's determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Per-weapon sway accumulator (POD) ───────────────────────────────────
// The live viewmodel sway offset — the ONLY mutable state here. `offset_x` is
// the horizontal (screen-X) drift and `offset_y` the vertical (screen-Y) drift,
// in the caller's chosen units (e.g. metres of local viewmodel translation, or
// normalised screen units). Each call eases these toward the look-driven target
// (which is 0 when the look is still, so the offset both follows a turn and
// recenters when it stops). Caller owns the storage. Trivially-copyable for
// snapshot / rollback.
struct SwayState {
    f32 offset_x = 0.0f;   // horizontal viewmodel sway (lags a yaw look)
    f32 offset_y = 0.0f;   // vertical viewmodel sway   (lags a pitch look)
};

// Layout assertion — 2 floats = 8 bytes, 4-byte aligned.
static_assert(sizeof(SwayState) == 8, "SwayState POD layout drifted");
static_assert(alignof(SwayState) <= 4, "SwayState should not over-align");

// ─── Sway tuning (POD) ───────────────────────────────────────────────────
// Authored per weapon / viewmodel (offline / data-driven). Pure tuning — no
// runtime state lives here, so a single SwayParams can be shared (by const&) by
// every weapon using that feel.
struct SwayParams {
    f32 sway_scale          = 0.01f;  // offset units per degree of look delta
    f32 max_offset          = 0.05f;  // clamp on |offset| (units), both axes
    f32 recenter_rate_per_s = 8.0f;   // how fast offset eases toward the target
};

// Layout assertion — 3 floats = 12 bytes, 4-byte aligned.
static_assert(sizeof(SwayParams) == 12, "SwayParams POD layout drifted");
static_assert(alignof(SwayParams) <= 4, "SwayParams should not over-align");

// ─── Sway operations (stateless aside from the SwayState) ────────────────

// Zero the offset: offset_x = offset_y = 0. Call once after choosing the
// SwayParams (e.g. on spawn / weapon-select) so the gun starts centred rather
// than easing in from a stale carried offset.
void sway_init(SwayState& s) noexcept;

// Advance one frame from this frame's look deltas (DEGREES). The gun LAGS the
// camera, so the target is the OPPOSITE sign of the look delta, scaled and
// clamped:
//
//     target_x = clamp(-look_yaw_delta_deg   * sway_scale, -max_offset, +max_offset);
//     target_y = clamp(-look_pitch_delta_deg * sway_scale, -max_offset, +max_offset);
//     t        = clamp(recenter_rate_per_s * dt_s, 0, 1);   // framerate-independent
//     offset  += (target - offset) * t;        // per axis: follow push AND recenter
//     offset   = clamp(offset, -max_offset, +max_offset);
//
// Because the same ease drives offset toward `target`, a held turn pushes the
// gun out (opposite the look) and a STILL look — where target is 0 — relaxes it
// back to centre, all with one relation. The blend factor `t` is clamped to
// [0, 1] so a single step never passes the target (no overshoot). A non-finite
// look delta (NaN / Inf from a bogus platform read) is treated as 0 for that
// axis, so it can never poison the offset. A non-finite or non-positive dt_s is
// guarded to a 0 blend (no move, no NaN), so a stalled / bogus frame clock
// leaves the sway untouched rather than corrupting it.
void sway_update(SwayState& s, const SwayParams& p, f32 look_yaw_delta_deg,
                 f32 look_pitch_delta_deg, f32 dt_s) noexcept;

// Snap the sway back to centred: offset_x = offset_y = 0 (e.g. on respawn /
// teleport / cutscene / weapon swap, to drop any carried-over drift). Equivalent
// to `s = SwayState{}` but spelled out for call-site clarity.
void sway_reset(SwayState& s) noexcept;

}  // namespace psynder::camera
