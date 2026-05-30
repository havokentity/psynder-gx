// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Shake.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe TRAUMA-based
// camera shake for the view. The impact/explosion analogue of Recoil's per-
// shot kick (see Recoil.h): where recoil is a directed spray climb, shake is
// an omnidirectional jolt that an explosion / heavy hit / landing pumps into
// the camera and that eases back out on its own.
//
// It produces ADDITIVE pitch/yaw/roll offsets (in DEGREES, in the same
// conventions as CameraState — see Camera.h) plus a small POSITIONAL punch (in
// METRES) that a caller folds onto the view each frame:
//
//     CameraState cam = ...;                 // integrated look angles
//     ShakeState  sh  = ...;                 // per-camera trauma accumulator
//
//     // ... on an explosion near the pawn:
//     shake_add_trauma(sh, 0.6f);
//
//     // ... every frame:
//     shake_tick(sh, dt_s, params);          // accumulate phase + decay trauma
//     const ShakeOffset o = shake_sample(sh, params);
//     cam.pitch_deg   += o.pitch_deg;        // jolts the view up/down
//     cam.yaw_deg     += o.yaw_deg;          // jolts the view left/right
//     // (roll + position are folded in by the view builder / a head bob node)
//
// Like Recoil, the shake does NOT clamp/wrap into [-89,89]/[-180,180] itself;
// it just emits the additive punch. The caller's own pitch-clamp / yaw-wrap
// (in update_camera) absorbs the offset once it is folded into the view angles,
// so even a full-trauma jolt can never punch the camera past the gimbal limits.
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - ShakeState / ShakeParams are PODs owned by the CALLER. No globals, no
//     singletons, no hidden statics. The API is a pure data transform on the
//     caller-owned ShakeState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a
//     ShakeState can be snapshotted into a replay / rollback buffer with a
//     plain memcpy.
//   - One ShakeState per camera; N states drive trivially in parallel from N
//     JobSystem workers (no shared mutable state).
//
// Determinism: the model uses ONLY +, -, *, / and an integer splitmix64 hash
// of (seed, axis, lattice_index) for the per-axis value noise — NO trigonometry
// and NO floating-point RNG state. macOS libm / glibc / MSVCRT disagree on
// sin/cos/tan at sub-ulp precision (Camera.h's note / Copilot PR #13), so the
// shake avoids them entirely: rather than the textbook sin()-based shake, the
// per-axis displacement is VALUE NOISE — hash the integer floor of the noise
// phase to a [-1,1] lattice value, hash floor+1, and smoothstep-lerp between
// them by the fractional phase. The same (state, params) yields a bit-identical
// ShakeOffset. The TU is compiled strict-FP (`-fno-fast-math -ffp-contract=off`
// / `/fp:strict`) so even the +,-,*,/ chain is reproducible across runs of the
// SAME platform, which is the lane's determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Per-camera trauma accumulator (POD) ─────────────────────────────────
// `trauma` is the current shake energy in [0, 1]; impacts add to it and it
// decays over time. `time_s` accumulates dt to drive the value-noise phase
// (it only ever grows — the noise is sampled at time_s * frequency_hz, so the
// wiggle advances even while trauma decays). `seed` selects the noise pattern
// so two cameras / two replays can use distinct shakes deterministically.
// Caller owns the storage. Trivially-copyable for snapshot / rollback.
struct ShakeState {
    f32 trauma = 0.0f;   // shake energy, clamped [0, 1]
    f32 time_s = 0.0f;   // accumulated phase clock for the noise (grows)
    u32 seed   = 0u;     // selects the per-axis noise pattern
};

// Layout assertion — 2 floats + 1 u32 = 3 * 4 = 12 bytes, 4-byte aligned.
static_assert(sizeof(ShakeState) == 12, "ShakeState POD layout drifted");
static_assert(alignof(ShakeState) <= 4, "ShakeState should not over-align");

// ─── Shake envelope tuning (POD) ─────────────────────────────────────────
// Authored per impact-class (offline / data-driven). Pure tuning — no runtime
// state lives here, so a single ShakeParams can be shared (by const&) by every
// camera using that envelope. The max_* fields are the PEAK displacement at
// trauma == 1 (the actual magnitude scales by trauma^2 — see shake_sample).
struct ShakeParams {
    f32 max_pitch_deg = 4.0f;    // peak additive view pitch at trauma==1 (+/-)
    f32 max_yaw_deg   = 4.0f;    // peak additive view yaw   at trauma==1 (+/-)
    f32 max_roll_deg  = 6.0f;    // peak additive view roll  at trauma==1 (+/-)
    f32 max_pos_m     = 0.05f;   // peak positional punch     at trauma==1 (+/-)
    f32 frequency_hz  = 20.0f;   // how fast the value noise wiggles (lattice/s)
    f32 decay_per_s   = 1.0f;    // trauma units shed per second
};

// ─── This-frame additive view punch (POD) ────────────────────────────────
// The shake offset to fold onto the camera this frame. Angles are DEGREES,
// position is METRES. All zero when trauma == 0.
struct ShakeOffset {
    f32 pitch_deg = 0.0f;
    f32 yaw_deg   = 0.0f;
    f32 roll_deg  = 0.0f;
    f32 pos[3]    = {0.0f, 0.0f, 0.0f};  // x, y, z metres
};

// ─── Shake operations (stateless aside from the ShakeState) ──────────────
// Add an impact's trauma, clamped into [0, 1] (multiple impacts in one frame
// stack but saturate — the shake can never exceed full energy). Negative
// `amount` is clamped away (treated as no smaller than 0 after the add), so a
// stray negative cannot drive trauma below zero. NaN/Inf amounts are ignored.
void shake_add_trauma(ShakeState& s, f32 amount) noexcept;

// Advance one frame: accumulate the noise phase clock (`time_s += dt_s`) and
// decay trauma toward rest (`trauma = max(0, trauma - decay_per_s * dt_s)`),
// clamping AT zero so it never goes negative. Non-positive dt_s is a no-op.
void shake_tick(ShakeState& s, f32 dt_s, const ShakeParams& p) noexcept;

// Sample the additive view punch for the CURRENT state. magnitude = trauma^2
// (a smooth ease-out — the shake fades gently as trauma drains rather than
// cutting off linearly). Each axis (pitch, yaw, roll, posX, posY, posZ) draws
// an INDEPENDENT deterministic value-noise sample in [-1, 1] at phase
// time_s * frequency_hz, then the offset for that axis is
// magnitude * max_<axis> * noise. At trauma == 0 every field is exactly 0.
// Pure +,-,*,/ + integer hash: same (state, params) => identical offset.
ShakeOffset shake_sample(const ShakeState& s, const ShakeParams& p) noexcept;

// Zero trauma + the phase clock (e.g. on respawn / scene cut). The `seed` is
// PRESERVED so the camera keeps its assigned noise pattern across resets;
// re-seed explicitly by assigning `s.seed` if a fresh pattern is wanted.
void shake_reset(ShakeState& s) noexcept;

// ─── Read helper ──────────────────────────────────────────────────────────
// The scalar shake intensity this frame (trauma^2), handy for driving an
// unrelated UI / audio reaction without re-sampling the full offset.
inline f32 shake_intensity(const ShakeState& s) noexcept {
    return s.trauma * s.trauma;
}

}  // namespace psynder::camera
