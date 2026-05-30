// SPDX-License-Identifier: MIT
//
// engine/audio/Compressor.h
//
// Lane 14 — master-bus peak compressor / limiter. A summed mix can momentarily
// sum past full scale (many in-phase sources, a transient explosion stacking on
// the music bed); feeding that straight to the device hard-clips and crackles.
// This is the safety net on the master bus: when the bus level exceeds a
// threshold it pulls a gain DOWN so the output sits at threshold + a softly
// compressed slice of the excess (the `ratio`), and a hard `ceiling` clamps the
// very loudest transients so the output amplitude can never exceed it (the
// limiter). A smoothed gain envelope (attack/release) eases the gain so the bus
// ducks fast onto a loud peak but recovers slowly, never pumping audibly.
//
// SCOPE: pure scalar gain math — no device I/O, no FFT, no per-sample buffer.
// The caller hands the compressor the block's peak/RMS level and dt each mix
// block, advances the envelope, then multiplies the master bus by
// compressor_gain(). Two layers, mirroring Ducking.h:
//   * compressor_target_gain() — the INSTANTANEOUS gain for a level (stateless).
//   * CompressorState + compressor_update() — the smoothed envelope that chases
//     that target with separate attack (down) / release (up) rates.
//
// Cosmetic (not the authoritative lockstep tick), so floats and the odd
// transcendental are fine — but this header needs none: it is pure +,-,*,/ and
// comparisons. Allocation-free, noexcept, deterministic (same-platform: no RNG,
// no wall-clock, stable evaluation order). Levels and gains are linear [0,1];
// rates are gain-units per second; dt is seconds. 1 unit = 1 m (unused here;
// stated for lane consistency).

#pragma once

#include "core/Types.h"

namespace psynder::audio {

// Static configuration of the master-bus compressor / limiter. POD —
// copy/compare freely.
//   threshold     — linear level in [0,1] above which compression begins. At or
//                   below threshold the signal passes untouched (gain 1.0).
//   ratio         — compression ratio, >= 1. 1.0 = no compression (the slice
//                   above threshold passes 1:1); higher ratios let through less
//                   of the excess (2.0 = half, 4.0 = a quarter, ...). A value
//                   below 1 is treated as 1 (never expand).
//   ceiling       — hard maximum output LEVEL in [0,1]: the limiter clamps the
//                   allowed output here so gain * input_level can never exceed
//                   it, however loud the input.
//   attack_per_s  — rate (gain units / second) the smoothed gain moves DOWN
//                   toward a lower (louder) target. Larger = clamps onto peaks
//                   faster.
//   release_per_s — rate (gain units / second) the smoothed gain moves UP
//                   toward a higher target (recovery). Larger = recovers faster.
struct CompressorParams {
    f32 threshold;      // [0,1]
    f32 ratio;          // >= 1
    f32 ceiling;        // [0,1]
    f32 attack_per_s;   // gain units / second, >= 0
    f32 release_per_s;  // gain units / second, >= 0
};

// The INSTANTANEOUS gain to apply to a signal at `input_level`, ignoring any
// smoothing. Stateless and deterministic.
//
// Behaviour:
//   * input_level <= threshold => 1.0 (below the knee, pass untouched).
//   * input_level >  threshold => compress the excess above the threshold by
//       `ratio`:   allowed_output = threshold + (input_level - threshold) / ratio
//     then the limiter clamps:    allowed_output = min(allowed_output, ceiling)
//     and the gain that maps this level onto that allowed output is
//                 gain = allowed_output / input_level.
//
// The result lies in (0, 1] for any well-formed (threshold <= ceiling) params:
// it only ever attenuates, never boosts. Guard: a non-positive input_level
// (silence / garbage) returns 1.0 (nothing to compress, no divide-by-zero). A
// ratio below 1 is treated as 1 (no expansion) and a ceiling below threshold is
// treated as the threshold so the band stays well-formed.
f32 compressor_target_gain(f32 input_level, const CompressorParams& p) noexcept;

// Smoothed gain envelope: the linear gain to multiply the master bus by. Lives
// in (0, 1]; 1.0 means no compression is currently applied. POD.
struct CompressorState {
    f32 gain;  // current smoothed linear gain, (0,1]
};

// Initialise an envelope to the un-compressed state (gain = 1.0).
void compressor_init(CompressorState& s) noexcept;

// Advance the smoothed gain envelope one mix block of `dt_s` seconds toward
// compressor_target_gain(input_level, p).
//
// Direction-dependent rate, mirroring a real compressor's attack/release knees:
//   * target BELOW the current gain (a louder peak demands more attenuation) =>
//     move DOWN at attack_per_s (clamp onto the peak quickly).
//   * target ABOVE the current gain (the signal dropped, recovering toward
//     unity) => move UP at release_per_s (ease back slowly so it never pumps).
// The move never overshoots — it snaps exactly onto the target rather than
// crossing it — and the result is clamped to (0, 1].
//
// Guard: a non-finite or non-positive `dt_s` (NaN/inf/<=0) makes NO move and
// leaves the envelope unchanged, so a stalled or garbage frame cannot poison the
// gain or run it backward. A non-finite or negative rate contributes no movement.
void compressor_update(CompressorState& s, const CompressorParams& p,
                       f32 input_level, f32 dt_s) noexcept;

// The smoothed linear gain to multiply the master bus by this block.
f32 compressor_gain(const CompressorState& s) noexcept;

}  // namespace psynder::audio
