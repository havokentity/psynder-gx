// SPDX-License-Identifier: MIT
//
// engine/audio/Crossfade.cpp — equal-power crossfade between two sources. See
// Crossfade.h.

#include "audio/Crossfade.h"

#include "math/Math.h"  // math::kHalfPi

#include <cmath>  // std::cos, std::sin, std::isfinite

namespace psynder::audio {

namespace {

// Clamp a position parameter to the crossfade domain [0,1].
inline f32 clamp01(f32 t) noexcept {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

}  // namespace

void equal_power_gains(f32 t, f32& out_a, f32& out_b) noexcept {
    const f32 c = clamp01(t);
    // Sweep an angle [0, pi/2] across the crossfade. cos starts at 1 (full A)
    // and falls to 0; sin starts at 0 and rises to 1 (full B); cos^2 + sin^2 is
    // identically 1, so the combined power is constant (no mid-fade dip).
    const f32 angle = c * math::kHalfPi;
    out_a = std::cos(angle);
    out_b = std::sin(angle);
}

void fader_init(Fader& f, f32 t) noexcept {
    f.t = clamp01(t);
}

void fader_update(Fader& f, f32 target_t, f32 rate_per_s, f32 dt_s) noexcept {
    // Guard against garbage timing / rates: only advance on a finite, positive
    // step with a finite, positive rate. Anything else holds position.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) return;
    if (!std::isfinite(rate_per_s) || rate_per_s <= 0.0f) return;

    const f32 target = clamp01(target_t);
    const f32 step = rate_per_s * dt_s;  // max distance to move this update

    if (f.t < target) {
        f.t += step;
        if (f.t > target) f.t = target;  // no overshoot past the target
    } else if (f.t > target) {
        f.t -= step;
        if (f.t < target) f.t = target;  // no overshoot past the target
    }

    f.t = clamp01(f.t);
}

void fader_gains(const Fader& f, f32& out_a, f32& out_b) noexcept {
    equal_power_gains(f.t, out_a, out_b);
}

f32 mix_two(f32 sample_a, f32 sample_b, const Fader& f) noexcept {
    f32 ga = 0.0f, gb = 0.0f;
    equal_power_gains(f.t, ga, gb);
    return sample_a * ga + sample_b * gb;
}

}  // namespace psynder::audio
