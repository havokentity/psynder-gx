// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/RangedDamage.cpp — see RangedDamage.h. Pure deterministic
// algebra layered on Ballistics; owns and mutates no state.

#include "gameplay/RangedDamage.h"

#include <algorithm>  // std::clamp

namespace psynder::gameplay {

f32 resolve_ranged_damage(f32 base_damage, f32 distance_m, Hitbox hb,
                          const FalloffProfile& p) noexcept {
    // Straight forwarding — unpack the profile and let Ballistics do every bit
    // of the falloff + hitbox math (and own every degenerate-input guarantee).
    return ranged_damage(base_damage, distance_m, p.falloff_start_m,
                         p.falloff_end_m, p.min_fraction, hb);
}

Hitbox classify_hitbox(f32 hit_y, f32 target_foot_y,
                       f32 target_height_m) noexcept {
    // A target with no positive vertical extent can't be regioned — fall back to
    // the Body baseline rather than dividing by zero / a negative height.
    if (target_height_m <= 0.0f) {
        return Hitbox::Body;
    }

    // Height up the target as a fraction of its height, clamped to [0, 1] so an
    // impact above the crown or below the feet bins into the nearest band.
    const f32 frac =
        std::clamp((hit_y - target_foot_y) / target_height_m, 0.0f, 1.0f);

    // Top band -> head, bottom band -> limb, the torso in between -> body.
    if (frac >= kHeadBandMin) {
        return Hitbox::Head;
    }
    if (frac <= kLimbBandMax) {
        return Hitbox::Limb;
    }
    return Hitbox::Body;
}

}  // namespace psynder::gameplay
