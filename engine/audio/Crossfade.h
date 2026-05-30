// SPDX-License-Identifier: MIT
//
// engine/audio/Crossfade.h
//
// Lane 14 — equal-power crossfade between two audio sources (music/ambience
// transitions). Blends from source A to source B over a normalised position
// `t` in [0,1] using the constant-power (sin/cos) law, so the COMBINED loudness
// stays constant through the transition — no mid-fade volume dip the way a
// naive linear crossfade has at t=0.5 (linear sums to 1.0 amplitude there;
// equal-power sums to ~1.414 amplitude / 1.0 power). A small timed `Fader`
// advances `t` toward a target at a fixed rate per second for smooth, frame-rate
// independent transitions.
//
// SCOPE: pure scalar mix math — no device I/O, no streaming, no resampling. POD
// in / POD out, branch-light, allocation-free, and `noexcept` so it is safe to
// call from the lock-free mixer pull path. Cosmetic (not the authoritative
// lockstep tick): same-platform deterministic. The sin/cos here are exactly the
// constant-power idea PositionalMix::pan_lr uses for stereo pan; no RNG, no
// wall-clock — `dt_s` is supplied by the caller's tick.
//
// Units: gains are linear [0,1]; `t` is dimensionless [0,1] (0 = full A, 1 =
// full B); `rate_per_s` is units-of-t per second; `dt_s` is seconds.

#pragma once

#include "core/Types.h"

namespace psynder::audio {

// ─── Equal-power crossfade gains ───────────────────────────────────────────
//
// Map a crossfade position `t` to a pair of linear gains for sources A and B
// using the constant-power law:
//   out_a = cos(t * pi/2),  out_b = sin(t * pi/2)   =>  out_a^2 + out_b^2 == 1.
//
// `t` is clamped to [0,1] first. At the endpoints and midpoint:
//   t = 0.0 -> out_a = 1, out_b = 0        (full A)
//   t = 1.0 -> out_a = 0, out_b = 1        (full B)
//   t = 0.5 -> out_a = out_b = 1/sqrt(2) ~ 0.7071  (equal, constant power)
// Because the squared gains always sum to 1, the perceived loudness of the
// crossfaded mix holds steady across the whole sweep (no centre dip).
void equal_power_gains(f32 t, f32& out_a, f32& out_b) noexcept;

// ─── Timed crossfade fader ─────────────────────────────────────────────────
//
// Holds the current crossfade position `t` in [0,1]:
//   t = 0 is full source A, t = 1 is full source B, in between is a blend.
// `fader_update` advances `t` toward a target at a fixed rate so a transition
// takes the same wall-time regardless of frame rate, and `fader_gains` reads
// out the equal-power gains at the current position.
struct Fader {
    f32 t;  // current crossfade position, [0,1]
};

// Initialise a fader to position `t`, clamped to [0,1].
void fader_init(Fader& f, f32 t) noexcept;

// Advance `f.t` toward `clamp(target_t, 0, 1)` by at most `rate_per_s * dt_s`
// (no overshoot — it lands exactly on the target when the step would pass it),
// then clamp the result to [0,1]. A non-finite or non-positive `dt_s`, or a
// non-finite/non-positive `rate_per_s`, leaves `f.t` unchanged (the fader only
// ever moves forward in time by a sane, finite step).
void fader_update(Fader& f, f32 target_t, f32 rate_per_s, f32 dt_s) noexcept;

// Read the equal-power gains at the fader's current position:
//   equal_power_gains(f.t, out_a, out_b).
void fader_gains(const Fader& f, f32& out_a, f32& out_b) noexcept;

// Convenience per-sample mix of two source samples at the fader's current
// position: out = sample_a * gain_a + sample_b * gain_b, where (gain_a, gain_b)
// are the equal-power gains for `f.t`. At t=0 returns `sample_a`, at t=1 returns
// `sample_b`.
f32 mix_two(f32 sample_a, f32 sample_b, const Fader& f) noexcept;

}  // namespace psynder::audio
