// SPDX-License-Identifier: MIT
//
// engine/audio/Ducking.cpp — sidechain ducking envelope. See Ducking.h.

#include "audio/Ducking.h"

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

void duck_init(DuckEnvelope& e) noexcept {
    e.gain = 1.0f;  // fully released — no ducking
}

f32 duck_target(const DuckParams& p, bool trigger_active) noexcept {
    // Trigger hot => head for the (clamped) floor; quiet => head for unity.
    return trigger_active ? clamp01(p.duck_floor) : 1.0f;
}

void duck_update(DuckEnvelope& e, const DuckParams& p, bool trigger_active,
                 f32 dt_s) noexcept {
    // Guard a stalled or garbage frame: a non-finite or non-positive dt makes
    // no move at all, so the gain cannot be poisoned by NaN/inf or run backward.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) {
        return;
    }

    const f32 floor = clamp01(p.duck_floor);
    const f32 target = trigger_active ? floor : 1.0f;

    // Pick the rate for the direction we are moving and the per-block step.
    // A non-finite or negative rate contributes no movement (treated as 0).
    f32 rate = trigger_active ? p.attack_per_s : p.release_per_s;
    if (!std::isfinite(rate) || rate < 0.0f) {
        rate = 0.0f;
    }
    const f32 step = rate * dt_s;

    f32 g = e.gain;
    if (g < target) {
        // Releasing upward toward unity (or rising toward a higher floor).
        g += step;
        if (g > target) g = target;  // never overshoot
    } else if (g > target) {
        // Ducking downward toward the floor.
        g -= step;
        if (g < target) g = target;  // never overshoot
    }
    // Already on target => nothing to do.

    // Keep the envelope inside its legal band regardless of the floor value.
    if (g < floor) g = floor;
    e.gain = clamp01(g);
}

f32 duck_gain(const DuckEnvelope& e) noexcept {
    return e.gain;
}

}  // namespace psynder::audio
