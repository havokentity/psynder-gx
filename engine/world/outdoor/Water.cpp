// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/Water.cpp — see Water.h.

#include "world/outdoor/Water.h"

#include <algorithm>  // std::clamp

namespace psynder::world::outdoor {

bool is_underwater(const WaterPlane& w, math::Vec3 pos) noexcept {
    return pos.y < w.level_m;
}

f32 submersion_depth(const WaterPlane& w, math::Vec3 pos) noexcept {
    const f32 d = w.level_m - pos.y;
    return d > 0.0f ? d : 0.0f;
}

f32 submersion_fraction(const WaterPlane& w, f32 body_bottom_y,
                        f32 body_height_m) noexcept {
    if (body_height_m <= 0.0f) {
        // Degenerate body: treat as a point at its bottom.
        return body_bottom_y < w.level_m ? 1.0f : 0.0f;
    }
    // Metres of the body below the surface, clamped to [0, height].
    const f32 below = w.level_m - body_bottom_y;
    const f32 submerged = std::clamp(below, 0.0f, body_height_m);
    return submerged / body_height_m;
}

f32 buoyancy_accel(f32 submersion_fraction, f32 gravity_m_s2,
                   f32 density_ratio) noexcept {
    const f32 f = std::clamp(submersion_fraction, 0.0f, 1.0f);
    return gravity_m_s2 * density_ratio * f;
}

f32 water_drag(f32 submersion_fraction, f32 velocity, f32 drag_coeff) noexcept {
    const f32 f = std::clamp(submersion_fraction, 0.0f, 1.0f);
    return -drag_coeff * f * velocity;  // opposes velocity
}

}  // namespace psynder::world::outdoor
