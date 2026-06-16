// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Recoil.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe weapon recoil /
// spray-pattern model for the view. Produces ADDITIVE pitch/yaw offsets (in
// DEGREES, in the same conventions as CameraState — see Camera.h) that a
// caller adds to the integrated view angles when building the camera each
// frame:
//
//     CameraState cam = ...;                 // integrated look angles
//     RecoilState  r  = ...;                 // per-weapon accumulator
//     cam.pitch_deg += recoil_pitch(r);      // kicks the view UP (positive)
//     cam.yaw_deg   += recoil_yaw(r);        // weaves left/right
//
// The recoil itself does NOT clamp/wrap into [-89,89]/[-180,180]; it just
// produces the additive kick. The caller's own pitch-clamp / yaw-wrap (in
// update_camera) applies once the offset is folded into the view angles, so
// the spray can never punch the camera past the gimbal limits.
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - RecoilState / RecoilPattern are PODs owned by the CALLER. No globals,
//     no singletons, no hidden statics. The API is a pure data transform on
//     the caller-owned RecoilState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a
//     RecoilState can be snapshotted into a replay / rollback buffer with a
//     plain memcpy.
//   - One RecoilState per weapon per pawn; N states drive trivially in
//     parallel from N JobSystem workers (no shared mutable state).
//
// Determinism: the model uses ONLY +, -, *, / and an integer splitmix64 hash
// of (pattern.seed, shot_index) for the horizontal jitter — NO trigonometry
// and NO floating-point RNG state. macOS libm / glibc / MSVCRT disagree on
// sin/cos/tan at sub-ulp precision (Camera.h's note / Copilot PR #13), so the
// recoil avoids them entirely: the same (pattern, shot sequence) yields
// bit-identical offsets. The TU is compiled strict-FP (`-fno-fast-math
// -ffp-contract=off` / `/fp:strict`) so even the +,-,*,/ chain is reproducible
// across runs of the SAME platform, which is the lane's determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Per-weapon recoil accumulator (POD) ─────────────────────────────────
// The current accumulated view kick plus how many shots deep we are into the
// CURRENT spray. Caller owns the storage. Trivially-copyable for snapshot /
// rollback. `shot_index` indexes the spray pattern: it advances per shot and
// resets to 0 only once the spray has fully recovered (so the next burst
// starts the pattern over from the top).
struct RecoilState {
    f32 pitch_offset_deg = 0.0f;   // additive view pitch, positive = up
    f32 yaw_offset_deg   = 0.0f;   // additive view yaw, +/- weave
    u32 shot_index       = 0;      // 0-based position in the current spray
};

// Layout assertion — 2 floats + 1 u32 = 3 * 4 = 12 bytes, 4-byte aligned.
static_assert(sizeof(RecoilState) == 12, "RecoilState POD layout drifted");
static_assert(alignof(RecoilState) <= 4, "RecoilState should not over-align");

// ─── Per-weapon recoil tuning (POD) ──────────────────────────────────────
// Authored per weapon (offline / data-driven). Pure tuning — no runtime
// state lives here, so a single RecoilPattern can be shared (by const&) by
// every pawn firing that weapon.
struct RecoilPattern {
    f32 pitch_per_shot_deg = 1.0f;    // vertical climb added per shot (up)
    f32 yaw_amplitude_deg  = 0.5f;    // horizontal jitter amplitude (+/-)
    f32 recovery_per_s_deg = 8.0f;    // degrees/second each offset decays
    f32 max_pitch_deg      = 12.0f;   // cap on accumulated pitch_offset_deg
    u32 seed               = 0u;      // seeds the horizontal weave pattern
};

// ─── Recoil operations (stateless aside from the RecoilState) ────────────
// Apply ONE shot's kick:
//   - shot_index += 1 (so the FIRST shot of a fresh spray uses index 0 for its
//     horizontal sample, then leaves shot_index == 1).
//   - pitch_offset_deg += pitch_per_shot_deg, clamped to max_pitch_deg (the
//     climb saturates; it never exceeds the cap and never decreases here).
//   - yaw_offset_deg   += yaw_amplitude_deg * signed_unit(hash(seed, idx)),
//     where signed_unit maps the splitmix64 hash bits to a deterministic value
//     in [-1, 1] with NO trig — so the horizontal kick weaves both ways.
// Pure +,-,*,/ + integer hash: same (pattern, shot order) => identical state.
void recoil_fire(RecoilState& s, const RecoilPattern& p) noexcept;

// Recover toward rest. Decays pitch_offset_deg and yaw_offset_deg toward 0 by
// `recovery_per_s_deg * dt_s`, moving EACH toward zero independently and
// clamping AT zero (never overshooting past it). When BOTH offsets have
// reached exactly 0, shot_index resets to 0 so the next shot restarts the
// spray pattern from the top. Non-positive dt_s is a no-op.
void recoil_recover(RecoilState& s, const RecoilPattern& p, f32 dt_s) noexcept;

// Zero everything (e.g. on weapon switch / respawn). Equivalent to
// `s = RecoilState{}` but spelled out for call-site clarity.
void recoil_reset(RecoilState& s) noexcept;

// ─── Read helpers ────────────────────────────────────────────────────────
// The additive view kick to fold into the camera's pitch_deg / yaw_deg.
inline f32 recoil_pitch(const RecoilState& s) noexcept { return s.pitch_offset_deg; }
inline f32 recoil_yaw  (const RecoilState& s) noexcept { return s.yaw_offset_deg; }

}  // namespace psynder::camera
