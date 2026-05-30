// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/LookSmoothing.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe MOUSE-LOOK
// SMOOTHER for the view: raw per-frame mouse deltas are jittery (sensor noise,
// uneven polling, integer-pixel quantisation), so an exponential low-pass
// filter glides them out before they integrate into yaw/pitch. The view then
// turns smoothly instead of stair-stepping, trading a touch of latency for
// steadiness. It is the input-side sibling of FovKick's dynamic FOV, Lean's
// peek slide, Recoil's per-shot kick, and ViewBob's gait sway (see FovKick.h /
// Lean.h / Recoil.h / ViewBob.h): a small POD accumulator the caller ticks
// each frame and feeds back into the camera.
//
//     CameraInput     ci = ...;             // lane 25's raw deltas this frame
//     LookSmoothState ss = ...;             // per-camera smoother accumulator
//     LookSmoothParams sp = ...;            // authored smoothing strength
//
//     // ... once, on spawn / camera-select:
//     look_smooth_init(ss);                 // zero the running velocities
//
//     // ... every frame, BEFORE update_camera (Camera.h):
//     f32 sy = 0.0f, sp_out = 0.0f;
//     look_smooth(ss, sp, ci.yaw_delta_deg, ci.pitch_delta_deg, sy, sp_out);
//     ci.yaw_delta_deg   = sy;              // feed the smoothed deltas onward
//     ci.pitch_delta_deg = sp_out;
//     update_camera(cam, ci);               // integrates the gentler deltas
//
// Purely cosmetic: nothing here feeds simulation, hit registration, or replay
// divergence beyond what the (already deterministic) camera integration does —
// it only shapes how the raw look deltas FEEL on their way into the view. It
// does NOT poll the platform, multiply sensitivity, or integrate yaw/pitch;
// it filters the two deltas in-place-by-out-param and leaves the rest to the
// caller, exactly as the other camera modifiers stay pure data transforms.
//
// Smoothing model (per-frame exponential low-pass):
//
//     alpha = 1 - clamp(smoothing, 0, 0.999)        // 0 < alpha <= 1
//     vel  += alpha * (raw - vel)                    // per axis
//     out   = vel
//
//   - smoothing == 0  => alpha == 1  => vel := raw  => out == raw EXACTLY
//     (pure passthrough, zero latency, zero added cost).
//   - higher smoothing => smaller alpha => the velocity creeps toward each
//     raw delta over several frames: laggier but steadier (less jitter).
//   - smoothing is clamped to [0, 0.999], so alpha is FLOORED at 0.001 and
//     can never reach 0. Even a pathological smoothing of 1 (or above) still
//     advances every frame — the smoother slows to a crawl but never freezes.
//
// NOTE — this is a PER-FRAME (framerate-dependent) smoother, the common feel
// for mouse-look smoothing in shipping FPS games: at a higher framerate the
// same `smoothing` settles faster in wall-clock time because more steps run
// per second. That is intentional and matches player expectation for an input
// feel knob. A dt-normalised variant (alpha derived from rate * dt, like
// FovKick/Lean's framerate-independent ease) is a deliberate follow-up; it is
// NOT used here so a smoothing of 0 stays a bit-exact passthrough.
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - LookSmoothState / LookSmoothParams are PODs owned by the CALLER. No
//     globals, no singletons, no hidden statics. The API is a pure data
//     transform on the caller-owned LookSmoothState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a
//     LookSmoothState can be snapshotted into a replay / rollback buffer with
//     a plain memcpy.
//   - One LookSmoothState per camera; N states drive trivially in parallel
//     from N JobSystem workers (no shared mutable state).
//
// Determinism: the model uses ONLY +, -, *, / and comparisons — NO trigonometry,
// NO floating-point RNG, NO wall-clock reads. macOS libm / glibc / MSVCRT
// disagree on sin/cos/tan at sub-ulp precision (Camera.h's note / Copilot
// PR #13), so the smoother avoids them entirely: the same (state, params,
// inputs) yields a bit-identical result. The TU is compiled strict-FP
// (`-fno-fast-math -ffp-contract=off` / `/fp:strict`) so even the +,-,*,/ chain
// is reproducible across runs of the SAME platform, which is the lane's
// determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Per-camera smoother accumulator (POD) ───────────────────────────────
// The running, low-pass-filtered look deltas in DEGREES — the ONLY mutable
// state here. Each call relaxes these toward the raw input and reads them back
// out. Caller owns the storage. Trivially-copyable for snapshot / rollback.
struct LookSmoothState {
    f32 yaw_vel_deg   = 0.0f;   // smoothed yaw delta carried across frames
    f32 pitch_vel_deg = 0.0f;   // smoothed pitch delta carried across frames
};

// Layout assertion — 2 floats = 8 bytes, 4-byte aligned.
static_assert(sizeof(LookSmoothState) == 8, "LookSmoothState POD layout drifted");
static_assert(alignof(LookSmoothState) <= 4, "LookSmoothState should not over-align");

// ─── Smoother tuning (POD) ───────────────────────────────────────────────
// Authored per camera / player (offline / data-driven, or wired to a settings
// slider). Pure tuning — no runtime state lives here, so a single
// LookSmoothParams can be shared (by const&) by every camera using that feel.
// A struct (not a bare f32) so future knobs — e.g. a dt-normalised mode or a
// per-axis strength — can land without breaking the call signature.
struct LookSmoothParams {
    f32 smoothing = 0.0f;   // 0 = raw passthrough; ->1 = heavily smoothed.
                            // Clamped to [0, 0.999] internally (see header).
};

// Layout assertion — a single f32 = 4 bytes, 4-byte aligned.
static_assert(sizeof(LookSmoothParams) == 4, "LookSmoothParams POD layout drifted");
static_assert(alignof(LookSmoothParams) <= 4, "LookSmoothParams should not over-align");

// ─── Smoother operations (stateless aside from the LookSmoothState) ──────

// Zero the running velocities: yaw_vel_deg = pitch_vel_deg = 0. Call once after
// choosing the LookSmoothParams (e.g. on spawn / camera-select) so the smoother
// starts settled rather than relaxing in from a stale carried delta.
void look_smooth_init(LookSmoothState& s) noexcept;

// Filter one frame's raw look deltas in place (via out-params). For each axis:
//
//     alpha = 1 - clamp(smoothing, 0, 0.999);   // 0.001 <= alpha <= 1
//     vel  += alpha * (raw - vel);
//     out   = vel;
//
// With smoothing == 0, alpha == 1 so vel := raw and out == raw EXACTLY (a
// bit-identical passthrough). Higher smoothing shrinks alpha so the velocity
// lags the raw delta over several frames (steadier, laggier). A non-finite raw
// delta (NaN / Inf) is treated as 0 for that axis, so a bogus platform reading
// can never poison the carried velocity. Reads the params by const&; mutates
// only `s` and writes the two out-params.
void look_smooth(LookSmoothState& s, const LookSmoothParams& p,
                 f32 raw_yaw_delta, f32 raw_pitch_delta,
                 f32& out_yaw, f32& out_pitch) noexcept;

// Snap the smoother back to settled: both velocities = 0 (e.g. on respawn /
// teleport / cutscene / sensitivity change, to drop any carried-over glide).
// Equivalent to `s = LookSmoothState{}` but spelled out for call-site clarity.
void look_smooth_reset(LookSmoothState& s) noexcept;

}  // namespace psynder::camera
