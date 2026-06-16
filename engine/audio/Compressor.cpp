// SPDX-License-Identifier: MIT
//
// engine/audio/Compressor.cpp — master-bus peak compressor / limiter. See
// Compressor.h.

#include "audio/Compressor.h"

#include <cmath>  // std::isfinite

namespace psynder::audio {

namespace {

// Clamp a linear gain into [0, 1].
inline f32 clamp01(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

}  // namespace

f32 compressor_target_gain(f32 input_level, const CompressorParams& p) noexcept {
    // Silence / garbage level: nothing to compress and no level to divide by.
    if (!(input_level > 0.0f)) {  // also rejects NaN
        return 1.0f;
    }

    // A well-formed knee: threshold in [0,1], ratio >= 1 (never expand), and a
    // ceiling that is at least the threshold (otherwise the band is inverted).
    const f32 threshold = clamp01(p.threshold);
    f32 ratio = p.ratio;
    if (!std::isfinite(ratio) || ratio < 1.0f) {
        ratio = 1.0f;  // no compression
    }
    f32 ceiling = clamp01(p.ceiling);
    if (ceiling < threshold) {
        ceiling = threshold;
    }

    // At or below the threshold the signal passes untouched.
    if (input_level <= threshold) {
        return 1.0f;
    }

    // Compress the slice ABOVE the threshold by the ratio, then hard-limit the
    // result to the ceiling. ratio == 1 leaves the slice intact (no compression
    // even above threshold); a higher ratio admits proportionally less of it.
    f32 allowed_output = threshold + (input_level - threshold) / ratio;
    if (allowed_output > ceiling) {
        allowed_output = ceiling;  // limiter: hard ceiling on the output level
    }

    // Map the input level onto the allowed output. Since allowed_output <=
    // input_level whenever we are above the threshold, the gain is in (0, 1].
    const f32 gain = allowed_output / input_level;
    return clamp01(gain);
}

void compressor_init(CompressorState& s) noexcept {
    s.gain = 1.0f;  // un-compressed — no attenuation
}

void compressor_update(CompressorState& s, const CompressorParams& p,
                       f32 input_level, f32 dt_s) noexcept {
    // Guard a stalled or garbage frame: a non-finite or non-positive dt makes no
    // move at all, so the gain cannot be poisoned by NaN/inf or run backward.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) {
        return;
    }

    const f32 target = compressor_target_gain(input_level, p);

    // Pick the rate for the direction we are moving: attack while clamping DOWN
    // onto a louder peak (target below the current gain), release while easing UP
    // on recovery (target above). A non-finite or negative rate makes no move.
    f32 g = s.gain;
    if (g > target) {
        // Attack: pull the gain down fast onto the louder target.
        f32 rate = p.attack_per_s;
        if (!std::isfinite(rate) || rate < 0.0f) {
            rate = 0.0f;
        }
        g -= rate * dt_s;
        if (g < target) g = target;  // never overshoot
    } else if (g < target) {
        // Release: ease the gain back up toward the quieter target (unity).
        f32 rate = p.release_per_s;
        if (!std::isfinite(rate) || rate < 0.0f) {
            rate = 0.0f;
        }
        g += rate * dt_s;
        if (g > target) g = target;  // never overshoot
    }
    // Already on target => nothing to do.

    s.gain = clamp01(g);
}

f32 compressor_gain(const CompressorState& s) noexcept {
    return s.gain;
}

}  // namespace psynder::audio
