// SPDX-License-Identifier: MIT
//
// engine/audio/PositionalMix.h
//
// Lane 14 — positional HDR audio mix math (ADR-023 / GitHub #47).
//
// This header is the deterministic, device-free core of the positional
// audio pipeline: how loud sources duck the noise floor (HDR gain
// windowing), how a source is panned across the stereo field
// (constant-power), and how geometry between source and listener
// attenuates and low-passes the signal (occlusion).
//
// SCOPE (this scaffold): pure mix math only — no device I/O, no rays,
// no HRTF dataset. The functions are header-inline, branch-light, POD
// in / POD out, allocation-free, and `noexcept` so they are safe to call
// from the lock-free mixer pull path. Determinism is a hard pillar:
// no wall-clock, no fast-math reordering hazards, stable evaluation order.
//
// TODO(ADR-023/#47): occlusion rays vs Jolt/ECS colliders (lane 15) + real
// HRTF dataset; tick-scheduled event queue. The `occlusion` scalar fed to
// occlusion_filter() will come from those rays; today it is supplied by
// the caller.
//
// Units: gains are linear [0,1]; loudness is dBFS (<= 0 is at/under full
// scale); azimuth is radians (0 = front, +x = right ear, -x = left ear);
// cutoff is Hz. 1 world unit = 1 metre (used by callers, not here).

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <cmath>
#include <span>

namespace psynder::audio {

// ─── Audible band the occlusion low-pass sweeps across ─────────────────────
// Fully open (no occlusion) passes the whole band; fully occluded clamps to a
// muffled cutoff. Endpoints are constants so the sweep is deterministic.
inline constexpr f32 kOcclusionMaxCutoffHz = 20000.0f;  // open: full audible band
inline constexpr f32 kOcclusionMinCutoffHz = 350.0f;    // sealed: heavy muffle

// ─── HDR perceptual mix gains ──────────────────────────────────────────────
//
// Compute a per-source linear gain such that the SUMMED output of all sources
// can never exceed full scale (no hard clipping), while preserving the
// perceptual ordering: louder sources keep more of the budget and quiet
// sources duck under them.
//
// `source_loudness_db` — per-source loudness in dBFS (typically <= 0).
// `out_gains`          — written 1:1 with `source_loudness_db` (same length).
// `headroom_db`        — extra safety margin below full scale (>= 0). A larger
//                        value pulls the whole mix down for transient headroom.
//
// Contract: with N sources, sum(out_gains[i] * lin(loudness[i])) <= full
// scale (1.0). Gains are in [0,1]. If the spans differ in size, the shorter
// length is used (no allocation, no throw).
//
// Method: convert each source to a linear amplitude, take the un-normalised
// sum of amplitudes, then scale every source by a single budget factor so the
// summed peak lands on the headroom-reduced ceiling. Because one common factor
// is applied, the relative (perceptual) balance between sources is preserved —
// loud sources dominate, quiet sources sit underneath — and the worst-case
// in-phase sum is exactly the ceiling, so it can never clip.
inline void hdr_gains(std::span<const f32> source_loudness_db,
                      std::span<f32>       out_gains,
                      f32                  headroom_db) noexcept {
    const usize n = source_loudness_db.size() < out_gains.size()
                        ? source_loudness_db.size()
                        : out_gains.size();
    if (n == 0) {
        return;
    }

    // Linear ceiling after pulling down by the requested headroom.
    const f32 hr = headroom_db < 0.0f ? 0.0f : headroom_db;
    const f32 ceiling = std::pow(10.0f, -hr / 20.0f);  // <= 1.0

    // Worst case (in-phase) summed amplitude = sum of per-source amplitudes.
    f32 sum_amp = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const f32 amp = std::pow(10.0f, source_loudness_db[i] / 20.0f);
        sum_amp += amp;
    }

    // Single budget factor: scale so the in-phase sum hits the ceiling. If the
    // sources are already quiet enough (sum <= ceiling) leave them at unity so
    // we never *boost* — HDR only ducks, never amplifies past authored level.
    f32 budget = 1.0f;
    if (sum_amp > ceiling && sum_amp > 0.0f) {
        budget = ceiling / sum_amp;
    }

    for (usize i = 0; i < n; ++i) {
        f32 g = budget;
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        out_gains[i] = g;
    }
}

// ─── Constant-power stereo pan ─────────────────────────────────────────────
//
// Map an azimuth angle to left/right gains using the constant-power
// (sin/cos) law, so a source swept across the field holds a steady perceived
// loudness (L^2 + R^2 == 1) with no centre dip.
//
// `azimuth_rad` — 0 = dead centre; negative = toward the left ear; positive =
//                 toward the right ear. Clamped to [-pi/2, +pi/2] (front
//                 hemisphere; full surround comes with the HRTF dataset TODO).
//
// At hard left  (-pi/2): out_left = 1, out_right = 0.
// At hard right (+pi/2): out_left = 0, out_right = 1.
// At centre        (0):  out_left = out_right = 1/sqrt(2).
inline void pan_lr(f32 azimuth_rad, f32& out_left, f32& out_right) noexcept {
    f32 az = azimuth_rad;
    if (az < -math::kHalfPi) az = -math::kHalfPi;
    if (az >  math::kHalfPi) az =  math::kHalfPi;

    // Remap azimuth [-pi/2, +pi/2] to a pan parameter t in [0, pi/2], where
    // t = 0 is hard left and t = pi/2 is hard right. Constant-power law:
    //   left  = cos(t), right = sin(t)  =>  left^2 + right^2 == 1.
    const f32 t = (az + math::kHalfPi) * 0.5f;  // [-pi/2,pi/2] -> [0,pi/2]
    out_left  = std::cos(t);
    out_right = std::sin(t);
}

// ─── Occlusion attenuation + low-pass ──────────────────────────────────────
//
// Geometry between source and listener makes a sound both quieter and duller
// (high frequencies are absorbed first). Map a normalised occlusion amount to
// a linear attenuation and a low-pass cutoff frequency.
//
// `occlusion`            — 0 = clear line of sight, 1 = fully blocked. Clamped.
// `out_attenuation`      — linear gain multiplier in [kOccludedAtten, 1].
//                          More occlusion => LOWER attenuation (quieter).
// `out_lowpass_cutoff_hz`— cutoff in Hz in [kOcclusionMinCutoffHz,
//                          kOcclusionMaxCutoffHz]. More occlusion => LOWER
//                          cutoff (more muffled).
inline void occlusion_filter(f32  occlusion,
                             f32& out_attenuation,
                             f32& out_lowpass_cutoff_hz) noexcept {
    f32 o = occlusion;
    if (o < 0.0f) o = 0.0f;
    if (o > 1.0f) o = 1.0f;

    // Linear amplitude floor for a fully-sealed source (about -26 dB): audible
    // but plainly behind geometry. Open = unity.
    constexpr f32 kOccludedAtten = 0.05f;
    out_attenuation = 1.0f - (1.0f - kOccludedAtten) * o;

    // Cutoff sweeps logarithmically (perceptually even) from the full band to
    // the muffled floor as occlusion rises.
    const f32 log_hi = std::log(kOcclusionMaxCutoffHz);
    const f32 log_lo = std::log(kOcclusionMinCutoffHz);
    out_lowpass_cutoff_hz = std::exp(log_hi + (log_lo - log_hi) * o);
}

}  // namespace psynder::audio
