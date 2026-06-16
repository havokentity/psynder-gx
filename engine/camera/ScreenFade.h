// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/ScreenFade.h
//
// Lane 16 — Camera + Input glue. A deterministic, replay-safe SCREEN-FADE
// envelope for the view: a full-screen overlay opacity that ramps UP to a
// colour (black on death / respawn, white on a flashbang), optionally HOLDS at
// full opacity, then ramps back DOWN to clear. It is the post / HUD-layer
// sibling of FovKick's dynamic FOV and Transition's pose blend (see FovKick.h /
// Transition.h): a small POD accumulator the caller ticks each frame and reads
// back — but instead of driving where the eye sits, it drives a single 0..1
// alpha that the HUD / post layer multiplies into a fullscreen quad.
//
//     FadeState  fade = ...;                   // per-camera fade accumulator
//     FadeParams fp   = ...;                   // authored in/hold/out + peak
//
//     // ... once, on setup:
//     fade_init(fade);                          // alpha 0, idle
//
//     // ... when the player dies:
//     fade_start(fade, fp);                     // begin fade-in (to black)
//
//     // ... every frame:
//     fade_update(fade, fp, dt_s);             // advance the envelope
//     const f32 a = fade_alpha(fade);           // overlay opacity this frame
//     draw_fullscreen_overlay(colour, a);       // HUD / post folds it in
//     if (!fade_active(fade)) { /* envelope done — clear screen */ }
//
// The COLOUR of the overlay (black, white, red) is the caller's / HUD's; this
// model owns only the timed ALPHA envelope. One FadeState drives one fade; a
// new fade_start re-arms it from the beginning.
//
// Envelope shape (phases): 0 = idle (alpha 0, inactive); 1 = fade-IN (alpha
// ramps 0 -> peak_alpha over fade_in_s); 2 = HOLD (alpha pinned at peak_alpha
// for hold_s); 3 = fade-OUT (alpha ramps peak_alpha -> 0 over fade_out_s); then
// back to idle. fade_update CARRIES the leftover dt across each phase boundary,
// so one large dt that overshoots a phase spills the remainder into the next —
// the envelope advances by exactly the elapsed time regardless of step size.
//
// Design (matches the lane's POD / DOTS-friendly contract — Camera.h):
//   - FadeState / FadeParams are PODs owned by the CALLER. No globals, no
//     singletons, no hidden statics. The API is a pure data transform on the
//     caller-owned FadeState reference.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable so a FadeState
//     can be snapshotted into a replay / rollback buffer with a plain memcpy.
//   - One FadeState per camera; N states drive trivially in parallel from N
//     JobSystem workers (no shared mutable state).
//
// Determinism: the model uses ONLY +, -, *, / and comparisons — NO trigonometry,
// NO floating-point RNG, NO wall-clock reads. macOS libm / glibc / MSVCRT
// disagree on sin/cos/tan at sub-ulp precision (Camera.h's note / Copilot
// PR #13), so the envelope avoids them entirely: the same (state, params, dt)
// yields a bit-identical alpha. The ramps are pure linear interpolation
// (add / multiply / divide) and the phase walk is pure subtract / compare — no
// fmod, no trig. The TU is compiled strict-FP (`-fno-fast-math
// -ffp-contract=off` / `/fp:strict`) so even the +,-,*,/ chain is reproducible
// across runs of the SAME platform, which is the lane's determinism guarantee.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ─── Phase identifiers ───────────────────────────────────────────────────
// Stored in FadeState::phase as a plain u32 (POD-friendly, snapshot-safe).
inline constexpr u32 kFadePhaseIdle = 0u;   // cleared, inactive
inline constexpr u32 kFadePhaseIn   = 1u;   // ramping 0 -> peak_alpha
inline constexpr u32 kFadePhaseHold = 2u;   // pinned at peak_alpha
inline constexpr u32 kFadePhaseOut  = 3u;   // ramping peak_alpha -> 0

// ─── Per-camera fade accumulator (POD) ───────────────────────────────────
// `alpha` is the live overlay opacity in [0, peak_alpha] — the value the caller
// multiplies into the fullscreen overlay each frame. `timer_s` is the elapsed
// clock WITHIN the current phase (resets to 0 at each phase boundary). `phase`
// is one of the kFadePhase* constants. Caller owns the storage.
// Trivially-copyable for snapshot / rollback.
struct FadeState {
    f32 alpha   = 0.0f;             // live overlay opacity, [0, peak_alpha]
    f32 timer_s = 0.0f;            // elapsed time within the current phase
    u32 phase   = kFadePhaseIdle;  // current envelope phase
};

// Layout assertion — 2 floats (8) + 1 u32 (4) = 12 bytes, 4-byte aligned.
static_assert(sizeof(FadeState) == 12, "FadeState POD layout drifted");
static_assert(alignof(FadeState) <= 4, "FadeState should not over-align");

// ─── Fade tuning (POD) ───────────────────────────────────────────────────
// Authored per fade event (death, respawn, flashbang). Pure tuning — no runtime
// state lives here, so a single FadeParams can be shared (by const&) by every
// fade using that feel. peak_alpha is the maximum opacity the envelope reaches
// (e.g. 1.0 for full black, lower for a translucent tint).
struct FadeParams {
    f32 fade_in_s   = 0.25f;   // seconds to ramp 0 -> peak_alpha
    f32 hold_s      = 0.0f;    // seconds pinned at peak_alpha
    f32 fade_out_s  = 0.5f;    // seconds to ramp peak_alpha -> 0
    f32 peak_alpha  = 1.0f;    // maximum overlay opacity, [0, 1]
};

// Layout assertion — 4 floats = 16 bytes, 4-byte aligned.
static_assert(sizeof(FadeParams) == 16, "FadeParams POD layout drifted");
static_assert(alignof(FadeParams) <= 4, "FadeParams should not over-align");

// ─── Fade operations (stateless aside from the FadeState) ────────────────

// Initialise to a cleared, idle envelope: alpha 0, timer 0, phase idle. Call
// once on setup so the overlay starts clear rather than from a stale value.
void fade_init(FadeState& s) noexcept;

// Begin a fade-in: reset the per-phase timer to 0 and enter phase IN, so the
// next fade_update starts ramping alpha 0 -> peak_alpha over fade_in_s.
//   - alpha is reset to 0 (the envelope always starts clear).
//   - If fade_in_s <= 0 (or non-finite) the fade-in is INSTANT: alpha jumps to
//     peak_alpha and the envelope enters phase HOLD straight away (timer 0), so
//     there is no visible ramp-in — useful for an immediate black-out that then
//     holds and fades back.
// A new fade_start always re-arms the envelope from the beginning, regardless of
// the current phase.
void fade_start(FadeState& s, const FadeParams& p) noexcept;

// Advance the envelope by dt_s:
//   - phase IN  : alpha ramps 0 -> peak_alpha over fade_in_s; on completion,
//                 enters HOLD carrying any leftover dt.
//   - phase HOLD: alpha pinned at peak_alpha for hold_s; on completion, enters
//                 OUT carrying any leftover dt.
//   - phase OUT : alpha ramps peak_alpha -> 0 over fade_out_s; on completion,
//                 enters IDLE (alpha 0) carrying any leftover dt (which is
//                 simply discarded once idle).
// The leftover dt is CARRIED across each phase boundary, so one large dt that
// overshoots a phase spills its remainder into the next — the envelope advances
// by exactly the elapsed time regardless of step size. A non-finite or
// non-positive dt_s is guarded (no advance, no NaN), so a stalled / bogus frame
// clock leaves the envelope untouched. Ticking an idle envelope is a no-op.
// alpha is always clamped to [0, peak_alpha].
void fade_update(FadeState& s, const FadeParams& p, f32 dt_s) noexcept;

// ─── Read helpers ──────────────────────────────────────────────────────────
// The live overlay opacity to fold into the fullscreen overlay this frame.
f32 fade_alpha(const FadeState& s) noexcept;

// True while the envelope is running (phase != idle). False once it has fully
// faded back out, or before any fade_start.
bool fade_active(const FadeState& s) noexcept;

}  // namespace psynder::camera
