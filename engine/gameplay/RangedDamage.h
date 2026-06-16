// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/RangedDamage.h — composable per-weapon ranged-damage profiles.
//
// A thin, additive layer on top of Ballistics: it bundles the three loose
// falloff parameters (start / end / floor) into a single POD profile, names a
// handful of canonical FPS weapon archetypes, and adds a vertical hit-region
// classifier so a future hitscan path can turn a flat weapon damage plus a ray
// impact point into distance- and hitbox-scaled damage with one call.
//
// Lockstep-safe — only +, -, *, / and std::clamp (through Ballistics), no
// transcendentals, no RNG, no allocation. The same f32 inputs yield bit-identical
// f32 output on every peer, every call.
//
// 1 world unit = 1 metre; all distances/heights are in metres, damage in hit
// points. This file owns no component state and edits nothing — it only composes
// the pure functions exported by Ballistics.h.

#pragma once

#include "core/Types.h"
#include "gameplay/Ballistics.h"  // Hitbox, ranged_damage

namespace psynder::gameplay {

// A weapon's distance-falloff curve, packing the three loose parameters that
// damage_at_distance() / ranged_damage() take. POD and trivially copyable so it
// can live in a weapon-stats table, an ECS component, or a constexpr table.
//
// The default-constructed profile is the *identity* profile: falloff_start_m = 0,
// falloff_end_m = 0 (i.e. end <= start) and min_fraction = 1. Fed through
// Ballistics that collapses to "full damage at every range" — a positive
// distance is > start (0), so the degenerate end<=start step fires and returns
// base_damage * min_fraction == base_damage * 1 == base_damage. A flat-damage
// weapon therefore maps 1:1 onto a default FalloffProfile with no special-casing.
struct FalloffProfile {
    f32 falloff_start_m = 0.0f;  // full damage at/under this range (metres)
    f32 falloff_end_m = 0.0f;    // floor reached at/after this range (metres)
    f32 min_fraction = 1.0f;     // long-range damage as a fraction of base [0,1]
};

// ---------------------------------------------------------------------------
// Canonical FPS weapon archetypes. Numbers are Quake / Battlefield-class metric
// values — playable map scales are ~10-100 m, so the ramps sit in that window.
// ---------------------------------------------------------------------------

// No falloff: full damage at any range. Maps an existing flat-damage weapon onto
// the profile API 1:1 (identity — see FalloffProfile's doc on why end<=start +
// min_fraction=1 collapses to full damage everywhere). Use for melee, rails, or
// any weapon authored before falloff existed.
inline constexpr FalloffProfile kNoFalloff{0.0f, 0.0f, 1.0f};

// Rifle / carbine: long, gentle falloff. Full damage out to 35 m, decaying to
// 60% of base by 90 m — a battle-rifle that still bites across a large outdoor
// engagement but rewards closing the distance.
inline constexpr FalloffProfile kRifleFalloff{35.0f, 90.0f, 0.6f};

// Pistol / sidearm: medium falloff. Full to 12 m, down to 45% by 35 m — strong
// in a duel, clearly outranged past mid-room.
inline constexpr FalloffProfile kPistolFalloff{12.0f, 35.0f, 0.45f};

// Shotgun: short range, harsh falloff. Full only to 6 m, collapsing to 15% of
// base by 18 m — devastating point-blank, near-useless across a courtyard.
inline constexpr FalloffProfile kShotgunFalloff{6.0f, 18.0f, 0.15f};

// ---------------------------------------------------------------------------
// Resolve / classify
// ---------------------------------------------------------------------------

// The one call combat code makes: flat base damage at a distance, against a
// hitbox, through a weapon's falloff profile. Pure forwarding to Ballistics'
// ranged_damage() with the profile's fields unpacked — no extra math, so it
// inherits every determinism / degenerate-input guarantee documented there.
f32 resolve_ranged_damage(f32 base_damage, f32 distance_m, Hitbox hb,
                          const FalloffProfile& p) noexcept;

// Region thresholds for classify_hitbox(), expressed as fractions of the
// target's height measured from the feet (0 = feet, 1 = crown):
//   frac >= kHeadBandMin (0.85)  -> Head  (top ~15% of the body)
//   frac <= kLimbBandMax (0.30)  -> Limb  (bottom ~30% — legs / lower arms)
//   otherwise                    -> Body  (the torso band in between)
inline constexpr f32 kHeadBandMin = 0.85f;
inline constexpr f32 kLimbBandMax = 0.30f;

// Classify which hitbox a ray struck from the impact height alone. Computes the
// normalised height fraction frac = clamp((hit_y - target_foot_y) /
// target_height_m, 0, 1) and bins it via the bands above. Pure algebra (one
// subtract, one divide, one clamp, two compares) — deterministic, no
// transcendentals. Degenerate guard: a non-positive target_height_m has no
// meaningful vertical extent, so we fall back to Body.
Hitbox classify_hitbox(f32 hit_y, f32 target_foot_y,
                       f32 target_height_m) noexcept;

}  // namespace psynder::gameplay
