// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Ballistics.cpp — see Ballistics.h. Pure deterministic algebra.

#include "gameplay/Ballistics.h"

#include <algorithm>  // std::clamp

namespace psynder::gameplay {

f32 hitbox_multiplier(Hitbox h) noexcept {
    switch (h) {
        case Hitbox::Head: return 2.0f;  // canonical 2x headshot
        case Hitbox::Limb: return 0.7f;  // ~30% extremity reduction
        case Hitbox::Body: return 1.0f;  // baseline
    }
    return 1.0f;  // unknown -> baseline
}

f32 damage_at_distance(f32 base_damage, f32 distance_m, f32 falloff_start_m,
                       f32 falloff_end_m, f32 min_fraction) noexcept {
    // Clamp the floor fraction into a sane [0, 1] range.
    const f32 frac = std::clamp(min_fraction, 0.0f, 1.0f);

    // Negative distance is treated as point-blank (0 m).
    const f32 d = distance_m < 0.0f ? 0.0f : distance_m;

    // The minimum (long-range) damage this weapon settles to.
    const f32 min_damage = base_damage * frac;

    // Inside the full-damage zone: no falloff yet.
    if (d <= falloff_start_m) {
        return base_damage;
    }

    // Degenerate / collapsed ramp: end at or before start => clean step. We are
    // already past falloff_start_m here, so we are at/after the step edge.
    if (falloff_end_m <= falloff_start_m) {
        return min_damage;
    }

    // At or beyond the falloff end: clamped to the minimum.
    if (d >= falloff_end_m) {
        return min_damage;
    }

    // Linear interpolation across the ramp. t in (0, 1).
    const f32 t = (d - falloff_start_m) / (falloff_end_m - falloff_start_m);
    return base_damage + (min_damage - base_damage) * t;
}

f32 ranged_damage(f32 base_damage, f32 distance_m, f32 falloff_start_m,
                  f32 falloff_end_m, f32 min_fraction, Hitbox hb) noexcept {
    return damage_at_distance(base_damage, distance_m, falloff_start_m,
                              falloff_end_m, min_fraction) *
           hitbox_multiplier(hb);
}

}  // namespace psynder::gameplay
