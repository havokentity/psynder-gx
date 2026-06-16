// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Lean.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe PEEK / LEAN model
// for the view: the player leans left/right to peek around cover. The view
// slides LATERALLY along the camera's right axis and ROLLS slightly toward the
// lean direction, easing smoothly between centred and the held lean rather than
// snapping. It is the lateral-peek sibling of Recoil's per-shot kick, FovKick's
// dynamic FOV, and ViewBob's gait sway (see Recoil.h / FovKick.h / ViewBob.h):
// a small POD accumulator the caller ticks each frame and folds back into the
// camera.
//
//     CameraState cam = ...;                 // the live view
//     LeanState   ls  = ...;                 // per-camera lean accumulator
//     LeanParams  lp  = ...;                 // authored slide + roll + ease
//
//     // ... every frame:
//     const f32 target = lean_input(key_q, key_e);       // -1 / 0 / +1
//     lean_update(ls, lp, target, dt_s);                 // ease toward it
//     const LeanOffset off = lean_sample(ls, lp);        // read the offset
//     // The caller applies the offset. CameraState has NO roll field, so the
//     // lateral slide is added to the world position along the camera's RIGHT
//     // axis, and the roll is folded into the view-matrix build:
//     const math::Vec3 right = right_vector(cam);
//     cam.position[0] += right.x * off.lateral_m;
//     cam.position[1] += right.y * off.lateral_m;
//     cam.position[2] += right.z * off.lateral_m;
//     // ... and off.roll_deg is passed to the caller's roll-aware view build.
//
// Purely cosmetic: nothing here feeds simulation, hit registration, or replay
// divergence — it only drives where the eye SITS and how the horizon TILTS.
// It does NOT itself displace the camera or check geometry: it produces the
// additive offset and leaves the apply (and any cover / wall-clip clamp) to the
// caller, exactly as Recoil produces an additive angle the caller folds in.
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - LeanState / LeanParams are PODs owned by the CALLER. No globals, no
//     singletons, no hidden statics. The API is a pure data transform on the
//     caller-owned LeanState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a
//     LeanState can be snapshotted into a replay / rollback buffer with a plain
//     memcpy.
//   - One LeanState per camera; N states drive trivially in parallel from N
//     JobSystem workers (no shared mutable state).
//
// Sign convention (held consistent across the whole API):
//   - amount/target/input -1 == full LEFT, +1 == full RIGHT, 0 == centred.
//   - Leaning RIGHT (amount > 0) yields a POSITIVE lateral_m — the eye slides
//     along the camera's +right axis (right_vector in Camera.h) — and a
//     POSITIVE roll_deg. Positive roll_deg tilts the horizon so the view banks
//     INTO the lean (the top of the screen rolls toward the right, the way a
//     head tilts when peeking right). Leaning LEFT mirrors both: negative
//     lateral_m (slide along -right) and negative roll_deg. At centre both are
//     exactly 0.
//
// Determinism: the model uses ONLY +, -, *, / and comparisons — NO trigonometry,
// NO floating-point RNG, NO wall-clock reads. macOS libm / glibc / MSVCRT
// disagree on sin/cos/tan at sub-ulp precision (Camera.h's note / Copilot
// PR #13), so the lean avoids them entirely: the same (state, params, inputs)
// yields a bit-identical result. The TU is compiled strict-FP (`-fno-fast-math
// -ffp-contract=off` / `/fp:strict`) so even the +,-,*,/ chain is reproducible
// across runs of the SAME platform, which is the lane's determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Per-camera lean accumulator (POD) ───────────────────────────────────
// `amount` is the live, eased lean in [-1, +1] — the ONLY mutable state here.
// -1 is full left, +1 is full right, 0 is centred. It eases toward the target
// fed to lean_update each frame. Caller owns the storage. Trivially-copyable
// for snapshot / rollback.
struct LeanState {
    f32 amount = 0.0f;   // eased lean, [-1, +1]; -1 left, +1 right, 0 centred
};

// Layout assertion — a single f32 = 4 bytes, 4-byte aligned.
static_assert(sizeof(LeanState) == 4, "LeanState POD layout drifted");
static_assert(alignof(LeanState) <= 4, "LeanState should not over-align");

// ─── Lean tuning (POD) ───────────────────────────────────────────────────
// Authored per camera / pawn (offline / data-driven). Pure tuning — no runtime
// state lives here, so a single LeanParams can be shared (by const&) by every
// camera using that feel. The peak values are reached at full lean (|amount|
// == 1) and scale linearly toward 0 at centre.
struct LeanParams {
    f32 max_lateral_m   = 0.4f;    // peak sideways slide at full lean, metres
    f32 max_roll_deg    = 8.0f;    // peak horizon roll at full lean, degrees
    f32 lerp_rate_per_s = 12.0f;   // how fast amount eases toward the target
};

// Layout assertion — 3 floats = 12 bytes, 4-byte aligned.
static_assert(sizeof(LeanParams) == 12, "LeanParams POD layout drifted");
static_assert(alignof(LeanParams) <= 4, "LeanParams should not over-align");

// ─── Additive view offset (POD) ──────────────────────────────────────────
// The displacement the caller folds into the view this frame: a lateral slide
// (metres) along the camera's right axis and a roll (degrees) of the horizon.
// See the file header's sign-convention block for the directions.
struct LeanOffset {
    f32 lateral_m = 0.0f;   // + slides along +right (lean right), - along -right
    f32 roll_deg  = 0.0f;   // + banks into a right lean, - into a left lean
};

// Layout assertion — 2 floats = 8 bytes, 4-byte aligned.
static_assert(sizeof(LeanOffset) == 8, "LeanOffset POD layout drifted");
static_assert(alignof(LeanOffset) <= 4, "LeanOffset should not over-align");

// ─── Lean operations (stateless aside from the LeanState) ────────────────

// Map the two held lean keys/buttons to a target lean in {-1, 0, +1}:
//   - left only  (lean_left && !lean_right)  => -1   (peek left)
//   - right only (!lean_left && lean_right)  => +1   (peek right)
//   - both held  OR neither held             =>  0   (centred — they cancel)
// Pure selection, no state touched. The {-1,0,+1} result is the `target` to
// pass straight into lean_update.
f32 lean_input(bool lean_left, bool lean_right) noexcept;

// Advance one frame: ease amount toward `target` (first CLAMPED to [-1, +1] so
// a bogus caller can never drive the eased lean out of range) by a framerate-
// independent approach:
//
//     t       = clamp(lerp_rate_per_s * dt_s, 0, 1);
//     amount += (clamped_target - amount) * t;
//
// Because the blend factor `t` is clamped to [0, 1], a single step moves AT
// MOST all the way to the target and NEVER past it (no overshoot); over many
// frames amount converges to the target. At t == 1 it snaps in one step.
// Non-finite or non-positive dt_s is guarded: it produces a 0 blend factor
// (no move, no NaN), so a stalled / bogus frame clock leaves the lean
// untouched rather than corrupting it. A non-finite target is also guarded
// (treated as a no-move) so NaN can never leak into amount.
void lean_update(LeanState& s, const LeanParams& p, f32 target, f32 dt_s) noexcept;

// Read the live additive offset for the current eased amount:
//     lateral_m = amount * max_lateral_m;   // + right, - left
//     roll_deg  = amount * max_roll_deg;     // + banks into a right lean
// Linear in amount: exactly 0 at centre, +/- the authored peak at full lean.
// Pure read — does not touch the state.
LeanOffset lean_sample(const LeanState& s, const LeanParams& p) noexcept;

// Snap the lean back to centred: amount = 0 (e.g. on respawn / cutscene /
// teleport). Equivalent to `s = LeanState{}` but spelled out for call-site
// clarity.
void lean_reset(LeanState& s) noexcept;

}  // namespace psynder::camera
