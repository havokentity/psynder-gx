// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainSlope.h — deterministic terrain slope +
// walkability queries on the heightfield, layered on HeightfieldQuery's
// surface normal. Used to validate spawn points and constrain pathing /
// movement to traversable ground on a Battlefield-light outdoor map.
//
// Determinism: `terrain_slope_updot` and `terrain_walkable` are pure ALGEBRAIC
// queries (they read the normal's Y and do one comparison) — IEEE-754
// deterministic across arm64 / x86_64 / MSVC under the lane's strict-FP flags,
// so they are safe on the lockstep tick. `terrain_slope_radians` calls acos()
// (a transcendental, NOT guaranteed bitwise-identical across platforms / libm
// implementations) and is therefore a QUERY / DEBUG helper only — callers that
// need cross-platform-bitwise lockstep parity MUST use `terrain_slope_updot`
// and compare against a PRECOMPUTED cos-threshold (i.e. `terrain_walkable`)
// rather than branching on the angle. Metric: world X/Z/Y in metres.

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "math/Math.h"
#include "core/Types.h"

namespace psynder::world::outdoor {

// "Up dot" = the terrain surface normal's Y component at world (wx, wz): 1.0 on
// perfectly flat ground, decreasing toward 0 as the surface steepens. Because
// the normal is unit length and the flat-ground reference normal is +Y, this is
// exactly cos(slope angle). Pure algebra over the heightfield — lockstep-safe.
f32 terrain_slope_updot(const HeightmapDesc& h, f32 wx, f32 wz) noexcept;

// Slope angle in radians at world (wx, wz) = acos(clamp(updot, -1, 1)): 0 on
// flat ground, pi/2 on a vertical face. NOTE: uses acos() (transcendental) and
// is a QUERY / DEBUG helper only — NOT bitwise-deterministic across platforms.
// For lockstep-safe steepness gating, use `terrain_slope_updot` /
// `terrain_walkable` and compare against a precomputed cos-threshold instead.
f32 terrain_slope_radians(const HeightmapDesc& h, f32 wx, f32 wz) noexcept;

// True when the ground at world (wx, wz) is traversable: updot >= min_updot.
// `min_updot` is cos(max_walkable_angle) — e.g. cos(45deg) ~ 0.707 admits any
// slope up to 45deg, a larger threshold (closer to 1) is stricter. The
// comparison is the LOCKSTEP-SAFE form (pure algebra, no transcendental), so
// this is the query to use when validating spawn points and constraining
// agent pathing / movement on the deterministic tick.
bool terrain_walkable(const HeightmapDesc& h, f32 wx, f32 wz,
                      f32 min_updot) noexcept;

}  // namespace psynder::world::outdoor
