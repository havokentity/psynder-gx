// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/Water.h — a flat water plane for a Battlefield-light
// outdoor map: submersion + buoyancy + drag queries. Water sits at a fixed world
// Y (the still surface); a point is underwater when below it. Buoyancy is the
// Archimedes upward acceleration on a partially/fully submerged body; drag is a
// simple linear resistance that grows with submersion + speed.
//
// Determinism (strict-FP world_outdoor lane): pure +,-,*,/ and std::clamp — no
// transcendentals, no RNG, no allocation. Same inputs => bit-identical. Metric:
// world Y, depths in metres; gravity in m/s^2.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

namespace psynder::world::outdoor {

// A still water surface at world Y == level_m.
struct WaterPlane {
    f32 level_m = 0.0f;
};

// True iff `pos` is below the water surface.
bool is_underwater(const WaterPlane& w, math::Vec3 pos) noexcept;

// Metres `pos` sits below the surface: max(0, level - pos.y). 0 at/above water.
f32 submersion_depth(const WaterPlane& w, math::Vec3 pos) noexcept;

// Fraction [0,1] of a vertical body submerged: the body spans [body_bottom_y,
// body_bottom_y + body_height_m]; returns how much of that span is below the
// water line. 0 fully above, 1 fully submerged, partial in between. A degenerate
// body (body_height_m <= 0) is treated as a point at its bottom (1 if below the
// surface, else 0).
f32 submersion_fraction(const WaterPlane& w, f32 body_bottom_y,
                        f32 body_height_m) noexcept;

// Upward buoyancy acceleration (m/s^2): gravity * density_ratio *
// clamp(submersion_fraction, 0, 1). `density_ratio` = fluid_density /
// body_density (> 1 floats, < 1 sinks); the NET vertical acceleration a caller
// applies is `buoyancy_accel - gravity` (this returns the pure buoyancy term).
// 0 when out of the water (fraction 0).
f32 buoyancy_accel(f32 submersion_fraction, f32 gravity_m_s2,
                   f32 density_ratio) noexcept;

// Linear water-drag acceleration OPPOSING `velocity`: -drag_coeff *
// clamp(submersion_fraction, 0, 1) * velocity. Grows with submersion and speed;
// returns 0 out of the water or at rest. (Add to the body's acceleration.)
f32 water_drag(f32 submersion_fraction, f32 velocity, f32 drag_coeff) noexcept;

}  // namespace psynder::world::outdoor
