// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Transition.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe POSE-BLEND model
// for the view: smoothly carry the camera from one pose to another over a fixed
// duration with an ease curve. It is the cinematic sibling of Recoil's per-shot
// kick, FovKick's dynamic FOV, and Lean's lateral peek (see Recoil.h /
// FovKick.h / Lean.h): a small POD accumulator the caller ticks each frame and
// reads back into the camera — but instead of easing toward a live target every
// frame, it interpolates a FIXED start→end pair over a known wall-clock span.
//
// Use it for death cams (snap the eye off the corpse and glide to a wide
// over-the-shoulder framing), kill cams (blend onto the victim and back),
// and spawn intros (sweep from an establishing pose down to the playable eye):
//
//     TransitionState t = ...;                  // per-cut blend accumulator
//     CamPose from = current_eye_pose(cam);     // where we are now
//     CamPose to   = killcam_pose(victim);      // where we want to land
//
//     transition_start(t, from, to, 1.25f);     // 1.25 s glide
//
//     // ... every frame, while the cut is live:
//     transition_tick(t, dt_s);                 // advance the clock
//     const CamPose p = transition_sample(t);   // current eased pose
//     cam.position[0] = p.position[0];          // fold it into the live view
//     cam.position[1] = p.position[1];
//     cam.position[2] = p.position[2];
//     cam.yaw_deg     = p.yaw_deg;
//     cam.pitch_deg   = p.pitch_deg;
//     if (!transition_active(t)) { /* hand control back to the player */ }
//
// Yaw wraparound: yaw_deg lives in [-180, +180] and wraps (Camera.h). A naive
// component lerp from +170 to -170 would sweep 340° the LONG way around through
// 0. transition_sample takes the SHORTEST ARC: it wraps the (to - from) delta
// into [-180, +180] so +170 → -170 passes through +180 (a 20° move), never
// through 0. The sampled yaw is then re-wrapped back into [-180, +180].
//
// Purely cosmetic: nothing here feeds simulation, hit registration, or replay
// divergence — it only drives where the eye SITS and which way it LOOKS during
// a cut. It produces a pose; the caller folds that pose into the live view and
// owns the decision of when the cut starts and when control returns.
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - CamPose / TransitionState are PODs owned by the CALLER. No globals, no
//     singletons, no hidden statics. The API is a pure data transform on the
//     caller-owned TransitionState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a
//     TransitionState can be snapshotted into a replay / rollback buffer with a
//     plain memcpy.
//   - One TransitionState per camera; N states drive trivially in parallel from
//     N JobSystem workers (no shared mutable state).
//
// Determinism: the model uses ONLY +, -, *, / and comparisons — NO trigonometry,
// NO floating-point RNG, NO wall-clock reads. macOS libm / glibc / MSVCRT
// disagree on sin/cos/tan at sub-ulp precision (Camera.h's note / Copilot
// PR #13), so the blend avoids them entirely: the same (state, inputs) yields a
// bit-identical pose. The smoothstep ease is pure polynomial (3x^2 - 2x^3) and
// the yaw shortest-arc wrap is pure add/subtract — no fmod, no trig. The TU is
// compiled strict-FP (`-fno-fast-math -ffp-contract=off` / `/fp:strict`) so even
// the +,-,*,/ chain is reproducible across runs of the SAME platform, which is
// the lane's determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Minimal camera pose (POD) ───────────────────────────────────────────
// The blend operates on a stripped-down pose — just the position + look angles
// — so it stays decoupled from CameraState's FOV / clip-plane fields (those do
// not interpolate during a cut). yaw_deg follows Camera.h's convention: degrees,
// canonical range [-180, +180], wraps. pitch_deg is degrees (the caller keeps
// its own clamp). Trivially-copyable.
struct CamPose {
    f32 position[3]{0.0f, 0.0f, 0.0f};   // metres, world space
    f32 yaw_deg   = 0.0f;                // around +Y, [-180, 180], wraps
    f32 pitch_deg = 0.0f;                // degrees (caller clamps)
};

// Layout assertion — position[3] + 2 floats = 5 floats = 20 bytes, 4-aligned.
static_assert(sizeof(CamPose) == 20, "CamPose POD layout drifted");
static_assert(alignof(CamPose) <= 4, "CamPose should not over-align");

// ─── Per-camera transition accumulator (POD) ─────────────────────────────
// Holds the fixed endpoints, the authored duration, and the elapsed clock. The
// ONLY mutable runtime field across a tick is elapsed_s (and active, which
// latches to 0 when the clock reaches the duration). Caller owns the storage.
// Trivially-copyable for snapshot / rollback.
struct TransitionState {
    CamPose from;                  // pose at elapsed 0
    CamPose to;                    // pose at elapsed >= duration
    f32     duration_s  = 0.0f;    // authored blend span, seconds
    f32     elapsed_s   = 0.0f;    // clock since start, clamped to duration
    u32     active      = 0u;      // 1 while blending, 0 when done / idle
};

// Layout assertion — 2 * CamPose (40) + 2 floats (8) + 1 u32 (4) = 52 bytes.
static_assert(sizeof(TransitionState) == 52, "TransitionState POD layout drifted");
static_assert(alignof(TransitionState) <= 4, "TransitionState should not over-align");

// ─── Transition operations (stateless aside from the TransitionState) ────

// Begin a blend: store the endpoints, reset the clock to 0, and mark active.
//   - A duration_s <= 0 (or non-finite) is degenerate: the endpoints are still
//     stored but active is set to 0 immediately, so transition_sample returns
//     `to` (an instant snap) and transition_active is false right away.
//   - Otherwise active = 1 and elapsed_s = 0 (transition_sample returns `from`
//     on the very next sample).
void transition_start(TransitionState& t, const CamPose& from, const CamPose& to,
                      f32 duration_s) noexcept;

// Advance one frame: add dt_s to elapsed_s, CLAMPED so the clock never exceeds
// duration_s. When elapsed_s reaches duration_s the blend is complete and active
// latches to 0. A non-finite or non-positive dt_s is guarded (no advance), so a
// stalled / bogus frame clock leaves the transition untouched rather than
// corrupting it. Ticking an inactive transition is a no-op.
void transition_tick(TransitionState& t, f32 dt_s) noexcept;

// Clamped smoothstep ease: x is first clamped to [0, 1], then mapped by the
// classic Hermite curve 3x^2 - 2x^3. Endpoints are exact: 0 at 0, 1 at 1, and
// 0.5 at 0.5; the curve has zero slope at both ends (the eye starts and lands
// gently). Pure polynomial algebra — NO trig, NO branch on the value.
f32 smoothstep01(f32 x) noexcept;

// Sample the current eased pose for the live clock:
//   u = (duration_s <= 0) ? 1 : smoothstep01(elapsed_s / duration_s);
//   - position: per-component lerp from.position -> to.position by u.
//   - yaw_deg : SHORTEST-ARC blend. The (to - from) yaw delta is wrapped into
//               [-180, +180], scaled by u, added to from.yaw_deg, then the
//               result is re-wrapped into [-180, +180]. So +170 -> -170 glides
//               through +180 (20°), never the long way through 0.
//   - pitch_deg: plain lerp by u (pitch does not wrap).
// At elapsed 0 this returns `from`; at/after duration it returns `to`. Pure read
// — does not touch the state.
CamPose transition_sample(const TransitionState& t) noexcept;

// True while the blend is still running (active == 1). Goes false once the clock
// reaches the duration, when the transition was started with a degenerate
// duration, or before any transition_start. Pure read.
bool transition_active(const TransitionState& t) noexcept;

}  // namespace psynder::camera
