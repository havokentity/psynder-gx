// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Ballistics.h — deterministic ranged-weapon ballistics.
//
// Pure metric algebra for ranged damage: distance-based damage falloff and
// per-hitbox (headshot / body / limb) damage multipliers. Lockstep-safe — only
// +, -, *, / and std::clamp, no transcendentals, no RNG, no allocation, no
// branches on platform state. The same f32 inputs yield bit-identical f32 output
// on every peer, every call.
//
// 1 world unit = 1 metre; all distances are in metres, damage in hit points.

#pragma once

#include "core/Types.h"

namespace psynder::gameplay {

// Which part of a target a shot connected with. u32-backed so it can ride in POD
// hit events / ECS components without padding surprises.
enum class Hitbox : u32 {
    Body = 0,  // torso / centre mass — the baseline (x1.0)
    Head = 1,  // headshot          — bonus damage (x2.0)
    Limb = 2,  // arm / leg         — reduced damage (x0.7)
};

// Damage scale for a hitbox. Canonical Quake / Battlefield-class values:
//   Body 1.0  (baseline),
//   Head 2.0  (the classic 2x headshot),
//   Limb 0.7  (the typical ~30% extremity damage reduction).
// Any out-of-range value falls back to the Body baseline (1.0).
f32 hitbox_multiplier(Hitbox h) noexcept;

// Distance damage falloff (deterministic linear interpolation):
//   distance <= falloff_start_m            -> base_damage (full)
//   falloff_start_m < d < falloff_end_m    -> linear lerp from base_damage down
//                                             to base_damage * min_fraction
//   distance >= falloff_end_m              -> base_damage * min_fraction (clamped)
//
// Degenerate-input guards (all deterministic, no transcendentals):
//   * negative distance is treated as 0 (full damage at the muzzle),
//   * min_fraction is clamped to [0, 1],
//   * if falloff_end_m <= falloff_start_m the ramp collapses to a clean step:
//     full damage strictly before falloff_start_m, base_damage * min_fraction
//     at or after it.
f32 damage_at_distance(f32 base_damage, f32 distance_m, f32 falloff_start_m,
                       f32 falloff_end_m, f32 min_fraction) noexcept;

// Full ranged-damage resolve: distance falloff scaled by the hitbox multiplier.
// Equivalent to damage_at_distance(...) * hitbox_multiplier(hb).
f32 ranged_damage(f32 base_damage, f32 distance_m, f32 falloff_start_m,
                  f32 falloff_end_m, f32 min_fraction, Hitbox hb) noexcept;

}  // namespace psynder::gameplay
