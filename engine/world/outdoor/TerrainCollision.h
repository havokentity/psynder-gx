// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainCollision.h — deterministic sphere-vs-heightfield
// collision resolve. Pushes a sphere (a rolling grenade, a ball, a simple
// character bound) out of the terrain along the surface normal and, optionally,
// reflects its velocity for a bounce.
//
// This is the *vertical-resolve* form: it samples the terrain height directly
// under the sphere centre (the same bilinear field the renderer/agents use, via
// HeightfieldQuery) and lifts the sphere so its bottom rests on the surface.
// That is correct for the common case (a sphere sitting on / falling onto
// ground that is locally near-flat under it) and is cheap and bit-stable. A
// full nearest-point-on-surface resolve (matters on steep faces / overhangs and
// near sharp ridges where the closest surface point is not straight below the
// centre) is a follow-up.
//
// Determinism: pure algebra plus sqrt (in `length` / `terrain_normal`) over the
// u16 height data — IEEE-754 deterministic across arm64 / x86_64 / MSVC under
// the lane's strict-FP flags. NO transcendentals (no acos). Safe to run on the
// deterministic lockstep tick: the same terrain + the same sphere yields a
// bit-identical resolve cross-platform. Metric: world X/Z/Y in metres.

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "math/Math.h"
#include "core/Types.h"

namespace psynder::world::outdoor {

// Result of a sphere-vs-terrain test (see `sphere_vs_terrain`).
struct SphereHit {
    bool       penetrating = false;            // true when the sphere is into the ground
    f32        penetration_m = 0.0f;           // metres of overlap (0 when not penetrating)
    math::Vec3 normal{0.0f, 1.0f, 0.0f};       // unit surface normal at the centre XZ
    math::Vec3 resolved_center{0.0f, 0.0f, 0.0f};  // centre after vertical push-out
};

// Test a sphere of `radius_m` centred at `center` against the heightfield.
// Samples the terrain height under the centre XZ; penetration is
//   (terrain_height + radius) - center.y.
// If penetration > 0 the sphere is into the ground:
//   penetrating      = true,
//   penetration_m    = penetration,
//   normal           = terrain_normal at the centre XZ,
//   resolved_center  = center lifted so its bottom rests on the surface
//                      (resolved_center.y = terrain_height + radius).
// Otherwise penetrating = false and resolved_center = center (unchanged).
//
// This is the simple vertical-resolve form (centre.y push-out along world up to
// seat the sphere bottom on the surface); a full nearest-point-on-surface
// resolve is a follow-up — see the file header.
SphereHit sphere_vs_terrain(const HeightmapDesc& h, math::Vec3 center,
                            f32 radius_m) noexcept;

// Standard reflection of `velocity` about a (unit) surface `normal`:
//   v' = v - (1 + restitution) * dot(v, normal) * normal.
// restitution 0 removes the normal component (slide / stop into the surface);
// restitution 1 is a perfect (energy-preserving) bounce; values in between
// damp the bounce. Pure: no terrain sampling.
math::Vec3 reflect_velocity(math::Vec3 velocity, math::Vec3 normal,
                            f32 restitution) noexcept;

// Resolve a sphere against the terrain in place. If `sphere_vs_terrain`
// penetrates, set `center` to the resolved (lifted) centre and `velocity` to
// `reflect_velocity(velocity, hit.normal, restitution)`, and return true.
// Otherwise leave `center` and `velocity` untouched and return false.
//
// Lockstep-safe: pure algebra + sqrt, no transcendentals.
bool resolve_sphere(const HeightmapDesc& h, math::Vec3& center,
                    math::Vec3& velocity, f32 radius_m,
                    f32 restitution) noexcept;

}  // namespace psynder::world::outdoor
