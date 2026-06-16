// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Knockback.h — deterministic radial knockback (blast shove).
//
// The velocity analog of Splash.h's radial damage: an explosion or melee/heavy
// hit shoves nearby entities AWAY from the blast point, scaled by distance —
// full `max_impulse` at the source, linearly falling to zero at `radius_m`,
// nothing at or beyond it. The push direction is the full 3D unit vector from
// the source to the target (horizontal + vertical), so a blast under your feet
// launches you upward, one to the side flings you sideways.
//
// There is no velocity component on the player pawn (the Jolt CharacterVirtual
// owns the player's motion — see ADR-019), so knockback is modelled two ways:
//   1. The pure impulse math — `knockback_impulse` — which the caller can read
//      and hand straight to whatever locomotion solver moves the entity
//      (Jolt body AddImpulse, the DOTS steering integrator, etc.).
//   2. A Knockback ECS component that accumulates the impulse as a velocity and
//      decays it over time, for movers that want the engine to carry the shove.
//
// 1 world unit = 1 metre. Impulse is expressed as m/s of velocity to add (an
// impulse divided by mass, i.e. a delta-v) so it composes directly with the
// metric velocity fields used elsewhere.
//
// Strict-FP / deterministic: pure algebra plus one sqrt for the normalize /
// distance. No RNG, no transcendentals — identical inputs give identical bits.

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT, scene::World

#include "math/Math.h"
#include "core/Types.h"

namespace psynder::gameplay {

// Pure radial-impulse math. Returns the delta-v (m/s) to push `target_pos` away
// from `source_pos`:
//
//   direction = normalize(target_pos - source_pos)        (full 3D, unit length)
//   magnitude = max_impulse * max(0, 1 - dist / radius_m) (linear falloff)
//
// Full `max_impulse` at the source, ramping linearly to 0 at `radius_m`, and
// exactly zero at or beyond `radius_m`. A non-positive `radius_m` yields the
// zero vector (a degenerate blast pushes nothing).
//
// AT-SOURCE DEGENERATE CASE: when `target_pos == source_pos` the direction is
// undefined (a zero-length vector cannot be normalized). We deliberately return
// the ZERO vector rather than a fabricated direction — a target exactly at the
// epicentre has no well-defined "away", and inventing one (e.g. straight up)
// would be an arbitrary, surprising launch. Callers that want a guaranteed pop
// at the centre can special-case dist == 0 themselves. This keeps the function
// branch-clean and never produces a NaN.
math::Vec3 knockback_impulse(math::Vec3 target_pos, math::Vec3 source_pos,
                             f32 max_impulse, f32 radius_m) noexcept;

// Accumulated knockback velocity (m/s) that decays toward rest. `damping_per_s`
// is the linear decay rate in (m/s) per second removed from each component's
// magnitude (a Quake-style ground-friction shove that bleeds off, NOT an
// exponential). A non-positive damping leaves the velocity untouched.
PSYNDER_COMPONENT(Knockback) {
    math::Vec3 velocity;       ///< current knockback delta-v, m/s, world space
    f32        damping_per_s;  ///< linear decay rate, (m/s) per second (>= 0)
};
static_assert(sizeof(Knockback) == 16, "Knockback layout frozen at 16 bytes");

// Add an impulse to the accumulated knockback velocity. Multiple blasts in the
// same tick compose additively (k.velocity += impulse).
void add_knockback(Knockback& k, math::Vec3 impulse) noexcept;

// Decay the knockback velocity toward zero by `damping_per_s * dt_s` per
// component, clamped so it never overshoots past 0 (no sign flip). Guards a
// non-finite or <= 0 dt (a no-op) and a non-positive damping (a no-op).
void tick_knockback(Knockback& k, f32 dt_s) noexcept;

// Detonate a knockback blast at `source`: for every entity carrying BOTH a
// Knockback and a TransformWS within `radius_m`, accumulate
// knockback_impulse(its_world_pos, source, max_impulse, radius_m) into its
// Knockback.velocity. Entities at/beyond the radius (and the at-source
// degenerate case) receive nothing.
//
// Deterministic: victims are gathered first, then mutated in ASCENDING
// entity-id order, so the chunk-iteration / spawn order can never affect the
// result. No RNG. One sqrt per affected entity.
void apply_radial_knockback(scene::World& w, math::Vec3 source, f32 max_impulse,
                            f32 radius_m) noexcept;

}  // namespace psynder::gameplay
