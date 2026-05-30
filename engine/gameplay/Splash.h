// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Splash.h — deterministic radial (blast) damage.
//
// On the authoritative lockstep tick: a projectile detonation (rocket / grenade)
// damages every Health entity inside an outer radius of the blast point, scaled
// by distance — full inside the inner radius, linearly falling to zero at the
// outer radius. No RNG, stable entity-id processing order, no per-call heap
// alloc beyond a reused gather buffer. The radial falloff reuses Ballistics'
// damage_at_distance, so the blast curve matches the rest of the damage model.
//
// 1 world unit = 1 metre; radii in metres, damage in hit points.

#pragma once

#include "scene/World.h"

#include "math/Math.h"
#include "core/Types.h"

#include <vector>

namespace psynder::gameplay {

// Radial blast shape. Full `max_damage` within `inner_radius_m`; linear falloff
// to 0 at `outer_radius_m`; nothing at or beyond the outer radius. Degenerate
// inputs follow damage_at_distance's guards (e.g. outer <= inner collapses the
// ramp to a clean step: full inside the inner radius, zero outside it).
struct SplashParams {
    f32 inner_radius_m;  // full-damage core radius (m)
    f32 outer_radius_m;  // damage reaches zero here (m)
    f32 max_damage;      // hit points at the centre / inside the inner radius
};

// Detonate a blast at `center` in world space, damaging every Health entity (with
// a TransformWS world position) whose 3D distance from the centre is within
// outer_radius_m. Per victim the damage is
//   damage_at_distance(p.max_damage, dist, inner_radius_m, outer_radius_m, 0.0f)
// (full inside the inner radius, ramping to 0 at the outer radius); a victim whose
// damage rounds to 0 is skipped. Damage is credited to `attacker`.
//
// `friendly_team` mirrors fire_hitscan's semantics: when >= 0, any victim carrying
// a Team component equal to it is spared; kNoTeam (-1, the default) disables the
// filter (free-for-all). The attacker is NOT auto-excluded — it is damaged by its
// own blast when in range (the classic rocket-jump).
//
// Deterministic: all victims are GATHERED first, then damage is applied in
// ascending entity-id order, so the iteration / spawn order can never affect the
// result. `scratch` is reused across calls (no per-call heap growth). Returns the
// number of victims actually damaged. One sqrt per candidate; no RNG, no
// transcendentals.
usize apply_splash_damage(scene::World& w, math::Vec3 center,
                          const SplashParams& p, Entity attacker,
                          i64 friendly_team, std::vector<Entity>& scratch) noexcept;

}  // namespace psynder::gameplay
