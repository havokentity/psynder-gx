// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/Wind.cpp — implementation of the cosmetic wind field.
// See Wind.h for the model and the determinism/scope contract (sin/cos here is
// same-platform deterministic but cross-platform-unsafe, hence cosmetic-only).

#include "world/outdoor/Wind.h"

#include <cmath>

namespace psynder::world::outdoor {

math::Vec3 wind_at(const WindParams& p, math::Vec3 world_pos,
                   f32 time_s) noexcept {
    // Steady heading. normalize() leaves a degenerate (zero) base_dir as-is.
    const math::Vec3 dir = math::normalize(p.base_dir);

    // Horizontal axis perpendicular to dir for the gust to buffet along. In the
    // XZ plane the left-perpendicular of (dx, _, dz) is (-dz, 0, dx); it is unit
    // length whenever dir's horizontal part is unit, which holds for a ~unit
    // horizontal base_dir.
    const math::Vec3 perp{-dir.z, 0.0f, dir.x};

    // Gust phase: oscillates in time and rolls across space via (x + z).
    const f32 phase = time_s * math::kTwoPi * p.gust_frequency_hz +
                      (world_pos.x + world_pos.z) * p.spatial_scale;
    const f32 gust = p.gust_strength * std::sin(phase);

    // wind = dir*base_strength + perp*gust.
    return math::add(math::mul(dir, p.base_strength), math::mul(perp, gust));
}

f32 wind_strength_at(const WindParams& p, math::Vec3 world_pos,
                     f32 time_s) noexcept {
    return math::length(wind_at(p, world_pos, time_s));
}

math::Vec3 wind_displacement(const WindParams& p, math::Vec3 world_pos,
                             f32 time_s, f32 sway_amount) noexcept {
    return math::mul(wind_at(p, world_pos, time_s), sway_amount);
}

}  // namespace psynder::world::outdoor
