// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Spring.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe CRITICALLY-DAMPED
// SMOOTHING SPRING for the view: it eases a scalar value toward a moving target
// WITH velocity, so it settles smoothly instead of snapping — and, being
// critically damped, it settles WITHOUT oscillation (no spring-y bounce, no
// overshoot ringing past the target). It is the velocity-carrying sibling of
// FovKick's exponential FOV ease, Lean's lateral peek, and Transition's fixed
// pose blend (see FovKick.h / Lean.h / Transition.h): a tiny POD accumulator
// the caller ticks each frame and reads back into the camera.
//
// This is the classic "SmoothDamp" used everywhere in third-person follow rigs:
//   - third-person camera follow distance / boom length chasing the pawn,
//   - aim-down-sights position lerps (slide the gun/eye toward the ADS pose),
//   - any smooth scalar chase where you want a soft, framerate-stable approach
//     that carries momentum (a value that's already moving keeps moving).
//
//     SpringState s = ...;                  // current value + its rate
//     spring_init(s, current_distance);     // seed, velocity 0
//
//     // ... every frame:
//     const f32 d = spring_update(s, target_distance,
//                                 /*smooth_time_s*/ 0.15f, dt_s);
//     cam_boom_length = d;                  // fold the eased value into the view
//
// `smooth_time_s` is the intuitive tuning knob: ROUGHLY the time (seconds) the
// value takes to reach the target — smaller is snappier, larger is floatier.
// Because the integrator is critically damped, the value approaches the target
// and settles; it does NOT overshoot meaningfully (the SmoothDamp formulation's
// only overshoot is a sub-epsilon numerical wisp, never a visible bounce).
//
// Algorithm — the Game Programming Gems 4 (§1.10, Thomas Lowe) "SmoothDamp"
// critically-damped spring, the semi-implicit form Unity ships as
// Mathf.SmoothDamp. It is a rational (Padé-style) approximation of the analytic
// critically-damped solution value(t) = target + (change + temp) * e^(-omega*t)
// that replaces the transcendental e^(-x) with the polynomial reciprocal
//     exp ≈ 1 / (1 + x + 0.48*x*x + 0.235*x*x*x),   x = omega * dt,
// so the whole step is pure +,-,*,/ — NO call to std::exp, NO trig (see the
// Determinism note below for why that matters). Per step, with
// omega = 2 / smooth_time:
//     x        = omega * dt;
//     exp      = 1 / (1 + x + 0.48*x*x + 0.235*x*x*x);
//     change   = value - target;                  // signed distance to go
//     temp     = (velocity + omega*change) * dt;
//     velocity = (velocity - omega*temp) * exp;    // new rate
//     value    = target + (change + temp) * exp;   // new value
// As dt (hence x) grows, exp -> 0, so value -> target and velocity -> 0: the
// chase always converges. A smaller smooth_time -> larger omega -> faster decay,
// i.e. it converges in fewer seconds.
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - SpringState is a POD owned by the CALLER. No globals, no singletons, no
//     hidden statics. The API is a pure data transform on the caller-owned
//     SpringState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a
//     SpringState can be snapshotted into a replay / rollback buffer with a
//     plain memcpy.
//   - One SpringState per chased value; N states drive trivially in parallel
//     from N JobSystem workers (no shared mutable state).
//
// Determinism: the model uses ONLY +, -, *, / and comparisons — NO trigonometry,
// NO std::exp, NO floating-point RNG, NO wall-clock reads. macOS libm / glibc /
// MSVCRT disagree on transcendentals (exp/sin/cos) at sub-ulp precision
// (Camera.h's note / Copilot PR #13), so the spring uses the polynomial exp
// approximation above and avoids them entirely: the same (state, target, dt)
// yields a bit-identical result. The TU is compiled strict-FP (`-fno-fast-math
// -ffp-contract=off` / `/fp:strict`) so even the +,-,*,/ chain is reproducible
// across runs of the SAME platform, which is the lane's determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Per-value spring accumulator (POD) ──────────────────────────────────
// `value` is the live, eased scalar (whatever the caller is chasing — a follow
// distance, an ADS offset, a smoothed sensitivity, ...). `velocity` is its rate
// of change, carried across frames so the chase has momentum; the integrator
// owns it and the caller normally only reads `value`. Both are mutable. Caller
// owns the storage. Trivially-copyable for snapshot / rollback.
struct SpringState {
    f32 value    = 0.0f;   // live eased value
    f32 velocity = 0.0f;   // its rate of change, carried across frames
};

// Layout assertion — 2 floats = 8 bytes, 4-byte aligned.
static_assert(sizeof(SpringState) == 8, "SpringState POD layout drifted");
static_assert(alignof(SpringState) <= 4, "SpringState should not over-align");

// ─── Spring operations (stateless aside from the SpringState) ────────────

// Seed the spring AT REST at `value`: value = value, velocity = 0. Call once
// before chasing (e.g. on spawn / weapon select) so it starts pinned to the
// current value rather than springing in from a stale one with stale momentum.
void spring_init(SpringState& s, f32 value) noexcept;

// Advance one frame: ease s.value toward `target` over ~`smooth_time_s` seconds
// using the critically-damped SmoothDamp integrator documented in the file
// header. Returns the NEW s.value (also written back into the state) for fold-in
// convenience.
//
// Guards (a bogus frame leaves the spring untouched — no NaN ever leaks in):
//   - dt_s non-finite or <= 0  -> no move, returns the current s.value.
//   - target non-finite        -> no move, returns the current s.value.
//   - smooth_time_s is clamped UP to a tiny positive floor before forming
//     omega = 2 / smooth_time, so a zero / tiny / non-finite smooth_time can't
//     divide-by-zero or blow omega to infinity; it just makes the chase very
//     snappy (a large-but-finite omega), converging in (nearly) one step.
//
// Convergence + overshoot: because exp = 1/(1 + x + 0.48 x^2 + 0.235 x^3) is in
// (0, 1] for x >= 0, each step contracts (change + temp) toward 0, so value ->
// target and velocity -> 0 over time. The spring is critically damped: it does
// NOT overshoot meaningfully — there is no oscillation past the target, only a
// sub-epsilon numerical wisp at most.
f32 spring_update(SpringState& s, f32 target, f32 smooth_time_s,
                  f32 dt_s) noexcept;

// True once the spring has effectively settled on `target`: BOTH the value is
// within `epsilon` of the target AND the velocity is within `epsilon` of 0
// (a value can momentarily pass through the target with high velocity, so both
// conditions matter). Pure read — does not touch the state.
bool spring_settled(const SpringState& s, f32 target, f32 epsilon) noexcept;

}  // namespace psynder::camera
