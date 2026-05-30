// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Melee.h — deterministic short-range cone melee attack.
//
// On the authoritative lockstep tick. melee_attack swings the attacker in a
// facing direction and strikes the NEAREST living enemy that lies both within
// `range_m` and inside the swing cone (dot(facing, toTarget) >= cos_half_angle),
// crediting `damage` to the attacker via the gameplay Damage system. Mirrors
// fire_hitscan's contract: friendly-team filtering, ascending-id tie-breaks, and
// strict-FP determinism. The per-attack path is pure +,-,*,/ + sqrt + the
// dot/cos compare — NO runtime trigonometry — so it stays lockstep-safe.

#pragma once

#include "gameplay/Weapons.h"  // kNoTeam (the canonical free-for-all sentinel)

#include "scene/World.h"

#include "math/Math.h"
#include "core/Types.h"

namespace psynder::gameplay {

// Swing a short-range cone melee from `attacker`. The cone is defined by its
// apex at `origin`, axis `facing`, half-angle (as a precomputed cosine
// `cos_half_angle`), and slant length `range_m`. Every Health entity (except the
// attacker) is a candidate; a candidate qualifies when:
//   * its centre is within `range_m` of `origin` (distance <= range_m), AND
//   * it lies inside the cone: dot(normalize(facing), normalize(toTarget))
//     >= cos_half_angle.
// The NEAREST qualifying enemy is struck (ties broken by lower entity id) and
// takes `damage`, credited to `attacker` (frag on a kill). Returns the victim,
// or an invalid Entity if nothing qualifies.
//
// `friendly_team` mirrors fire_hitscan: when >= 0, any candidate carrying a Team
// equal to it is skipped (a teammate in the swing is passed over, the enemy
// behind them can still be hit). kNoTeam (-1, the default) keeps free-for-all.
//
// `cos_half_angle` is a COSINE THRESHOLD, not an angle: cos(0) == 1 is a needle
// (only dead-ahead), cos(90deg) == 0 is a flat half-space, cos(180deg) == -1 is
// omnidirectional. Passing a cosine (not an angle) keeps the hot path free of
// acos/trig so it is bit-identical across platforms. Degenerate inputs
// (non-positive range, zero-length facing, or a zero-length toTarget at the
// origin) are guarded and yield no hit. One sqrt per candidate (to normalize
// toTarget) is fine — determinism only forbids transcendental trig, not sqrt.
Entity melee_attack(scene::World& w, Entity attacker, math::Vec3 origin,
                    math::Vec3 facing, f32 range_m, f32 cos_half_angle,
                    f32 damage, i64 friendly_team = kNoTeam) noexcept;

// SETUP / AUTHORING helper: convert a cone half-angle in DEGREES to the cosine
// threshold melee_attack expects. Uses std::cos, so it is a one-off authoring /
// configuration convenience — call it when building weapon data, NOT on the
// per-tick hot path. (Runtime trig is not guaranteed bit-identical across
// platforms/compilers, which would break lockstep; the lane keeps all trig at
// setup time and feeds the precomputed cosine into the deterministic core,
// exactly as fire_hitscan takes a pre-converted spread_tan.)
f32 melee_cone_cos(f32 half_angle_deg) noexcept;

}  // namespace psynder::gameplay
