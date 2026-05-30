// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/FovKick.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe DYNAMIC FOV model
// for the view: the cosmetic "kick" that widens the field of view while the
// pawn sprints and narrows it while aiming down sights (ADS), easing smoothly
// between the two rather than snapping. It is the FOV analogue of Recoil's
// per-shot kick and Shake's trauma jolt (see Recoil.h / Shake.h): a small POD
// accumulator the caller ticks each frame and reads back into the camera.
//
//     CameraState cam = ...;                 // the live view
//     FovState    fk  = ...;                 // per-camera FOV accumulator
//     FovParams   fp  = ...;                 // authored resting/sprint/ADS FOVs
//
//     // ... once, after picking fp:
//     fov_init(fk, fp);                       // current_deg = base_deg
//
//     // ... every frame:
//     fov_update(fk, fp, sprinting, aiming, dt_s);
//     cam.fov_y_deg = fov_value(fk);          // write the eased FOV into the view
//
// Purely cosmetic: nothing here feeds simulation, hit registration, or replay
// divergence — it only drives what the player SEES through the lens. It does
// NOT clamp the FOV to any sane on-screen range; the authored base/sprint/ads
// values ARE the range, and current_deg only ever eases BETWEEN them (it can
// never pass the target — see fov_update), so a well-authored FovParams keeps
// the on-screen FOV bounded by construction.
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - FovState / FovParams are PODs owned by the CALLER. No globals, no
//     singletons, no hidden statics. The API is a pure data transform on the
//     caller-owned FovState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a
//     FovState can be snapshotted into a replay / rollback buffer with a plain
//     memcpy.
//   - One FovState per camera; N states drive trivially in parallel from N
//     JobSystem workers (no shared mutable state).
//
// Determinism: the model uses ONLY +, -, *, / and comparisons — NO trigonometry,
// NO floating-point RNG, NO wall-clock reads. macOS libm / glibc / MSVCRT
// disagree on sin/cos/tan at sub-ulp precision (Camera.h's note / Copilot
// PR #13), so the FOV kick avoids them entirely: the same (state, params,
// inputs) yields a bit-identical result. The TU is compiled strict-FP
// (`-fno-fast-math -ffp-contract=off` / `/fp:strict`) so even the +,-,*,/ chain
// is reproducible across runs of the SAME platform, which is the lane's
// determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Per-camera FOV accumulator (POD) ────────────────────────────────────
// `current_deg` is the live, interpolated vertical field of view in DEGREES —
// the value the caller writes into CameraState.fov_y_deg each frame. It eases
// toward the active target (see fov_target) and is the ONLY mutable state here.
// Caller owns the storage. Trivially-copyable for snapshot / rollback.
struct FovState {
    f32 current_deg = 60.0f;   // live interpolated FOV, degrees
};

// Layout assertion — a single f32 = 4 bytes, 4-byte aligned.
static_assert(sizeof(FovState) == 4, "FovState POD layout drifted");
static_assert(alignof(FovState) <= 4, "FovState should not over-align");

// ─── FOV tuning (POD) ────────────────────────────────────────────────────
// Authored per camera / weapon (offline / data-driven). Pure tuning — no
// runtime state lives here, so a single FovParams can be shared (by const&) by
// every camera using that feel. Typical authoring: sprint_deg > base_deg >
// ads_deg (sprint widens, ADS narrows), though the model does not REQUIRE that
// ordering — whatever values are authored are the values eased toward.
struct FovParams {
    f32 base_deg         = 60.0f;   // resting FOV (not sprinting, not aiming)
    f32 sprint_deg       = 75.0f;   // widened FOV while sprinting
    f32 ads_deg          = 45.0f;   // narrowed FOV while aiming down sights
    f32 lerp_rate_per_s  = 10.0f;   // how fast current eases toward the target
};

// Layout assertion — 4 floats = 16 bytes, 4-byte aligned.
static_assert(sizeof(FovParams) == 16, "FovParams POD layout drifted");
static_assert(alignof(FovParams) <= 4, "FovParams should not over-align");

// ─── FOV operations (stateless aside from the FovState) ──────────────────

// The FOV the view should ease toward for the given pawn state. PRIORITY:
// aiming beats sprinting beats resting. If `aiming` is true the target is
// ads_deg (ADS always wins — you can't sprint while scoped); else if
// `sprinting` is true the target is sprint_deg; else the resting base_deg.
// Pure selection, no state touched.
f32 fov_target(const FovParams& p, bool sprinting, bool aiming) noexcept;

// Initialise the live FOV to the resting value: current_deg = base_deg. Call
// once after choosing the FovParams (e.g. on spawn / weapon select) so the
// camera starts at rest rather than easing in from a stale value.
void fov_init(FovState& s, const FovParams& p) noexcept;

// Advance one frame: ease current_deg toward fov_target(p, sprinting, aiming)
// by a FRAMERATE-INDEPENDENT exponential-ish approach:
//
//     t        = clamp(lerp_rate_per_s * dt_s, 0, 1);
//     current += (target - current) * t;
//
// Because the blend factor `t` is clamped to [0, 1], a single step moves AT
// MOST all the way to the target and NEVER past it (no overshoot); over many
// frames current_deg converges to the target. A larger lerp_rate_per_s (or a
// larger dt) converges faster — at t == 1 it snaps in one step. Non-finite or
// non-positive dt_s is guarded: it produces a 0 blend factor (no move, no NaN),
// so a stalled / bogus frame clock leaves the FOV untouched rather than
// corrupting it.
void fov_update(FovState& s, const FovParams& p, bool sprinting, bool aiming,
                f32 dt_s) noexcept;

// ─── Read helper ──────────────────────────────────────────────────────────
// The live FOV in degrees to fold into CameraState.fov_y_deg this frame.
inline f32 fov_value(const FovState& s) noexcept { return s.current_deg; }

}  // namespace psynder::camera
