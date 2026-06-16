// SPDX-License-Identifier: MIT
//
// engine/audio/Footsteps.h
//
// Lane 14 — distance-driven footstep cadence timing.
//
// This header decides WHEN to emit the next footstep sound from how far the
// pawn has actually travelled, rather than from a wall-clock timer. A step
// fires every `stride_length_m` metres of accumulated travel, so faster
// movement produces a faster cadence, slower movement a slower one, and a
// standing-still pawn (zero travel) is silent. Because the cadence is keyed to
// distance and not to elapsed time, it is framerate-independent: the same path
// walked over 10 frames or 100 frames emits the same number of steps.
//
// The caller passes the stride each call, so sprinting can use a longer stride
// (a bigger bound between steps) while a crouch-walk can use a shorter one,
// without this code knowing anything about movement state.
//
// SCOPE: pure cadence bookkeeping — no clip selection, no spatialisation, no
// device I/O. Whichever sound to play (surface material, left/right foot) and
// where to place it is the caller's job; this only answers "is it time for the
// next step?". POD in / POD out, allocation-free, `noexcept`.
//
// DETERMINISM (a hard pillar): same-platform deterministic. Only +, -, *, /;
// no trig, no RNG, no wall-clock. Identical input sequences => identical step
// patterns, so a lockstep replay reproduces the exact footstep timeline.
//
// Units: 1 world unit = 1 metre, so `distance_moved_m` and `stride_length_m`
// are metres. Audio is cosmetic, so f32 is fine here.

#pragma once

#include "core/Types.h"

namespace psynder::audio {

// ─── Per-pawn footstep cadence state ───────────────────────────────────────
//
// POD. One instance lives per walking entity (the local player, each remote
// pawn, an AI bot, ...). `distance_accum_m` is the distance banked toward the
// next step; it stays in [0, stride) between steps because every completed
// stride is subtracted off (the remainder carries forward, so cadence does not
// drift). `step_count` is the running total of steps emitted since the last
// reset — useful for alternating the left/right foot or cycling clip variants.
struct FootstepState {
    f32 distance_accum_m;  // distance banked toward the next step, in metres
    u32 step_count;        // total steps emitted since init/reset
};

// Zero both fields: no banked distance, no steps yet.
inline void footsteps_init(FootstepState& s) noexcept {
    s.distance_accum_m = 0.0f;
    s.step_count       = 0;
}

// Identical to init — provided as a named "reset" for clarity at call sites
// that re-arm the cadence (e.g. on respawn or teleport so the first step after
// the jump does not fire early from stale banked distance).
inline void footsteps_reset(FootstepState& s) noexcept {
    s.distance_accum_m = 0.0f;
    s.step_count       = 0;
}

// Advance the cadence by one movement increment and report whether a footstep
// should be emitted this call.
//
// `distance_moved_m`  — metres travelled since the previous call. Negatives are
//                       clamped to 0 (you cannot "un-walk" a step), so a sign
//                       glitch never rewinds the cadence.
// `stride_length_m`   — metres of travel between consecutive steps. A larger
//                       value spaces steps further apart (sprint); a smaller
//                       value packs them tighter (crouch-walk).
//
// Behaviour:
//   - Adds the (clamped) distance to `distance_accum_m`.
//   - While the bank has reached a full stride, subtracts one stride and
//     increments `step_count`.
//   - Returns true if at least one step fired this call, false otherwise.
//
// MULTI-STRIDE CHOICE: this is the RECOMMENDED looping form. A single large
// move (a big dt, a lag-induced catch-up, a short dash) that covers N whole
// strides emits N steps in one call: the loop subtracts a stride and bumps the
// count N times, and the leftover (< one stride) carries forward in
// `distance_accum_m`. `step_count` therefore always reflects the true number
// of strides walked, never under-counting on a frame spike. The return value
// is a simple "did anything fire" flag; read `step_count` (e.g. diff it across
// calls) if you need the exact number that fired this call.
//
// Guard: if `stride_length_m <= 0` the stride is degenerate (a divide/loop
// hazard), so nothing is accumulated, no step fires, and the state is left
// completely unchanged. Returns false.
inline bool footstep_advance(FootstepState& s,
                             f32            distance_moved_m,
                             f32            stride_length_m) noexcept {
    // Degenerate stride: leave state untouched, emit nothing.
    if (stride_length_m <= 0.0f) {
        return false;
    }

    // Clamp negative travel to zero — movement only ever advances the cadence.
    f32 moved = distance_moved_m;
    if (moved < 0.0f) {
        moved = 0.0f;
    }

    s.distance_accum_m += moved;

    // Emit as many whole strides as fit; carry the remainder forward. The
    // loop is bounded because each pass removes a strictly positive stride
    // from a finite bank.
    bool fired = false;
    while (s.distance_accum_m >= stride_length_m) {
        s.distance_accum_m -= stride_length_m;
        s.step_count += 1u;
        fired = true;
    }
    return fired;
}

// Fraction of the way to the next step, in [0, 1). Useful for driving a
// footstep-anticipation cue (e.g. a faint scuff that swells as the foot is
// about to land). Because `distance_accum_m` is kept below one stride by
// `footstep_advance`, the ratio stays under 1.0.
//
// Guard: `stride_length_m <= 0` yields 0.0f (no meaningful progress).
inline f32 footstep_progress(const FootstepState& s,
                             f32                  stride_length_m) noexcept {
    if (stride_length_m <= 0.0f) {
        return 0.0f;
    }
    return s.distance_accum_m / stride_length_m;
}

}  // namespace psynder::audio
