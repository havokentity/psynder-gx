// SPDX-License-Identifier: MIT
//
// engine/audio/Attenuation.cpp
//
// Lane 14 — selectable distance-attenuation rolloff models (see Attenuation.h
// for the per-model formulas, units, and the shared edge-case contract).

#include "audio/Attenuation.h"

#include <cmath>  // std::exp, std::log10 (libstdc++ on CI needs the explicit include)

namespace psynder::audio {

namespace {

inline f32 clamp01(f32 g) noexcept {
    if (g < 0.0f) return 0.0f;
    if (g > 1.0f) return 1.0f;
    return g;
}

}  // namespace

f32 attenuation(RolloffModel model,
                f32          distance_m,
                f32          ref_dist_m,
                f32          max_dist_m,
                f32          rolloff_factor) noexcept {
    f32 d = distance_m;
    if (d < 0.0f) d = 0.0f;  // distance can never be negative

    // Inner plateau: at/under the reference radius the source is at unity. This
    // takes priority over the max cutoff so a degenerate range (max <= ref) still
    // reports full gain AT ref (the documented "full gain at/under ref" contract).
    if (d <= ref_dist_m) {
        return 1.0f;
    }

    // Hard cutoff: at/beyond max the source is silent. This also realises the
    // degenerate-range (max <= ref) fallback — any distance past ref is then
    // >= max, so it returns 0 here.
    if (d >= max_dist_m) {
        return 0.0f;
    }

    // Here ref < d < max, so span = (max - ref) > 0 and x = (d - ref) > 0.
    f32 f = rolloff_factor;
    if (f < 0.0f) f = 0.0f;  // steepness is non-negative

    const f32 x    = d - ref_dist_m;
    const f32 span = max_dist_m - ref_dist_m;  // > 0 (max > ref here)

    f32 g;
    switch (model) {
        case RolloffModel::Linear:
            // Straight line from 1.0 at ref to 0.0 at max (== 0.5 at midpoint).
            g = 1.0f - x / span;
            break;

        case RolloffModel::Inverse:
            // Textbook inverse distance: g = ref / (ref + f*x). ref > 0 here, so
            // the denominator is >= ref > 0.
            g = ref_dist_m / (ref_dist_m + f * x);
            break;

        case RolloffModel::InverseSquare:
            // Steeper, physical-ish: g = ref^2 / (ref^2 + f*x^2). Denominator
            // >= ref^2 > 0.
            g = (ref_dist_m * ref_dist_m) /
                (ref_dist_m * ref_dist_m + f * x * x);
            break;

        case RolloffModel::Exponential:
            // Exponential decay normalised over the band: g = exp(-f * x/span).
            // At x == span (i.e. d == max) this is exp(-f) before the hard
            // cutoff above forces exact silence.
            g = std::exp(-f * (x / span));
            break;

        default:
            // Unknown tag — fall back to the linear curve rather than UB.
            g = 1.0f - x / span;
            break;
    }

    return clamp01(g);
}

f32 attenuation_db(RolloffModel model,
                   f32          distance_m,
                   f32          ref_dist_m,
                   f32          max_dist_m,
                   f32          rolloff_factor) noexcept {
    const f32 g =
        attenuation(model, distance_m, ref_dist_m, max_dist_m, rolloff_factor);

    // A true zero gain is -infinity dB; floor sub-epsilon gains to a large
    // finite negative so the result is always orderable. 1e-6 linear is already
    // below the -120 dB floor, so the threshold and the floor agree.
    if (g <= 1e-6f) {
        return kAttenuationDbFloor;
    }
    return 20.0f * std::log10(g);
}

}  // namespace psynder::audio
