// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Formation.cpp — deterministic squad formation slots. See Formation.h.

#include "ai/Formation.h"

#include "math/Math.h"

namespace psynder::ai {

namespace {

// Horizontal (XZ) unit forward, falling back to the canonical -Z look when the
// projected forward is degenerate (zero-length, e.g. looking straight up/down).
math::Vec3 forward_xz(math::Vec3 forward) noexcept {
    math::Vec3 f{forward.x, 0.0f, forward.z};
    if (math::length(f) <= 0.0f) {
        return {0.0f, 0.0f, -1.0f};
    }
    return math::normalize(f);
}

}  // namespace

math::Vec3 formation_slot(math::Vec3 leader_pos, math::Vec3 leader_forward,
                          FormationShape shape, u32 member_index,
                          f32 spacing_m) noexcept {
    // Member 0 always occupies the leader's own slot, for every shape.
    if (member_index == 0) {
        return leader_pos;
    }

    const math::Vec3 fwd = forward_xz(leader_forward);
    // right = cross(forward, +Y) = (-fwd.z, 0, fwd.x); already unit since fwd is
    // a unit horizontal vector (matches Camera.cpp right_vector handedness:
    // forward (0,0,-1) -> right (+1,0,0), forward (1,0,0) -> right (0,0,1)).
    const math::Vec3 right{-fwd.z, 0.0f, fwd.x};
    const math::Vec3 back = math::mul(fwd, -1.0f);  // directly behind the leader

    const u32 i = member_index;
    switch (shape) {
        case FormationShape::Line: {
            // Alternate right (odd i) / left (even i); rank = ceil(i/2).
            const u32 rank = (i + 1u) / 2u;
            const f32 side = (i % 2u == 1u) ? 1.0f : -1.0f;
            return math::add(leader_pos,
                             math::mul(right, side * static_cast<f32>(rank) * spacing_m));
        }
        case FormationShape::Column: {
            // Single file straight behind, i ranks back.
            return math::add(leader_pos,
                             math::mul(back, static_cast<f32>(i) * spacing_m));
        }
        case FormationShape::Wedge:
        default: {
            // A V: step ceil(i/2) back AND out, side alternating right/left.
            const u32 step = (i + 1u) / 2u;
            const f32 side = (i % 2u == 1u) ? 1.0f : -1.0f;
            const math::Vec3 b = math::mul(back, static_cast<f32>(step) * spacing_m);
            const math::Vec3 l = math::mul(right, side * static_cast<f32>(step) * spacing_m);
            return math::add(leader_pos, math::add(b, l));
        }
    }
}

void formation_slots(math::Vec3 leader_pos, math::Vec3 leader_forward,
                     FormationShape shape, u32 count, f32 spacing_m,
                     std::vector<math::Vec3>& out) noexcept {
    out.resize(count);
    for (u32 i = 0; i < count; ++i) {
        out[i] = formation_slot(leader_pos, leader_forward, shape, i, spacing_m);
    }
}

}  // namespace psynder::ai
