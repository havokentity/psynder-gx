// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/ViewBob.cpp — see ViewBob.h.
//
// UNLIKE Recoil.cpp / Shake.cpp (which deliberately avoid <cmath>), this TU
// DOES use std::sin: the walk-cycle bob is a pure cosmetic waveform folded onto
// the VIEW, never into the authoritative lockstep tick, so a sub-ulp libm
// divergence cannot desync gameplay. The lane's strict-FP build
// (`-fno-fast-math -ffp-contract=off` / `/fp:strict`) still makes the bob
// bit-identical across runs of the SAME platform; cross-platform bit-identity
// is not claimed for the trig (see ViewBob.h's determinism note).

#include "camera/ViewBob.h"

#include <cmath>

namespace psynder::camera {

namespace {

// 2*pi as an f32 constant — one full walk cycle in phase radians.
constexpr f32 kTwoPi = 6.28318530717958647692f;

// True iff finite (no extra headers): NaN != itself; Inf - Inf is NaN, so the
// self-subtraction trick rejects both NaN and the infinities in one test.
// Matches the is_finite idiom in Shake.cpp.
bool is_finite(f32 x) noexcept {
    return (x == x) && ((x - x) == 0.0f);
}

// Wrap a phase into [0, 2*pi) WITHOUT std::fmod, so the wrap itself stays pure
// +,-,*,/ and exactly reproducible on the strict-FP build. Each call to
// bob_advance adds a single non-negative step (distance and cycle_len are both
// guarded > 0 before we get here), so the running phase can exceed 2*pi by at
// most one step; a small subtract-loop brings it back into range. The loop is
// bounded defensively in case a caller passes an enormous single step.
f32 wrap_two_pi(f32 phase) noexcept {
    // Bring a (possibly large) positive phase down into [0, 2*pi).
    for (int guard = 0; phase >= kTwoPi && guard < 1024; ++guard) {
        phase -= kTwoPi;
    }
    // Defensive: a stray negative (should not arise from a non-negative step)
    // is nudged back up so the state stays in canonical range.
    for (int guard = 0; phase < 0.0f && guard < 1024; ++guard) {
        phase += kTwoPi;
    }
    return phase;
}

}  // namespace

void bob_advance(BobState& s, f32 distance_travelled_m, const BobParams& p) noexcept {
    // Guard the divisor and the input: a non-positive cycle length or a
    // non-finite distance leaves the phase UNTOUCHED — no divide-by-zero, no
    // NaN/Inf leaking into the persistent state.
    if (!(p.cycle_len_m > 0.0f)) return;          // also rejects NaN cycle_len
    if (!is_finite(distance_travelled_m)) return;

    // A stopped (or rearward-zeroed) pawn does not advance the bob. We treat a
    // negative distance as "no forward travel this tick" so the phase only ever
    // moves with real planar locomotion the caller measured.
    if (distance_travelled_m <= 0.0f) return;

    // Phase advances proportionally to distance: one full 2*pi cycle per
    // `cycle_len_m` metres travelled, so faster movement bobs faster.
    const f32 step = kTwoPi * (distance_travelled_m / p.cycle_len_m);
    s.phase = wrap_two_pi(s.phase + step);
}

BobOffset bob_sample(const BobState& s, const BobParams& p) noexcept {
    BobOffset out{};

    const f32 phase = s.phase;

    // Figure-8 head motion (see ViewBob.h):
    //   vertical bounces at DOUBLE the lateral frequency (sin(2*phase)) — one
    //   dip per foot-fall — while the lateral sway and the roll share the
    //   single-frequency stride (sin(phase)).
    out.pos[0] = p.lateral_amp_m  * std::sin(phase);          // lateral X sway
    out.pos[1] = p.vertical_amp_m * std::sin(2.0f * phase);   // vertical Y bounce
    out.pos[2] = 0.0f;                                        // no fore/aft bob
    out.roll_deg = p.roll_amp_deg * std::sin(phase);          // view roll
    return out;
}

void bob_reset(BobState& s) noexcept {
    s.phase = 0.0f;
}

}  // namespace psynder::camera
