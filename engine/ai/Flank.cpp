// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Flank.cpp — deterministic flanking-position computation. See Flank.h.

#include "ai/Flank.h"

#include "math/Math.h"

namespace psynder::ai {

namespace {

// Horizontal (XZ) unit forward, falling back to the canonical -Z look when the
// projected forward is degenerate (zero-length, e.g. a facing straight up/down).
// Mirrors Formation.cpp's forward_xz so the two share the same handedness.
math::Vec3 forward_xz(math::Vec3 facing) noexcept {
    math::Vec3 f{facing.x, 0.0f, facing.z};
    if (math::length(f) <= 0.0f) {
        return {0.0f, 0.0f, -1.0f};
    }
    return math::normalize(f);
}

// Unit offset DIRECTION (XZ, y == 0) for a flank side given the target's unit
// forward. right = (-fwd.z, 0, fwd.x) = cross(forward, +Y).
math::Vec3 side_offset_dir(math::Vec3 fwd, FlankSide side) noexcept {
    const math::Vec3 right{-fwd.z, 0.0f, fwd.x};
    switch (side) {
        case FlankSide::Right:
            return right;
        case FlankSide::Left:
            return math::mul(right, -1.0f);
        case FlankSide::Rear:
        default:
            return math::mul(fwd, -1.0f);
    }
}

}  // namespace

math::Vec3 flank_position(math::Vec3 target_pos, math::Vec3 target_facing,
                          FlankSide side, f32 distance_m) noexcept {
    const math::Vec3 fwd = forward_xz(target_facing);
    const math::Vec3 dir = side_offset_dir(fwd, side);
    // Offset in the target's XZ plane; dir.y == 0 keeps y == target_pos.y.
    return math::add(target_pos, math::mul(dir, distance_m));
}

FlankSide best_flank_side(math::Vec3 attacker_pos, math::Vec3 target_pos,
                          math::Vec3 target_facing) noexcept {
    const math::Vec3 fwd = forward_xz(target_facing);

    // Direction from the target toward the attacker, in the XZ plane. The side
    // whose offset direction best aligns with this (largest dot) is the side the
    // attacker is already nearest to, so it needs the least repositioning.
    const math::Vec3 to_att_raw{attacker_pos.x - target_pos.x, 0.0f,
                                attacker_pos.z - target_pos.z};
    const math::Vec3 to_att = math::normalize(to_att_raw);

    // Evaluate sides in enum order (Left, Right, Rear) and keep the strictly
    // largest alignment, so equal scores keep the earlier side => Left < Right <
    // Rear tie-break. When the attacker coincides with the target (zero-length
    // to_att), every dot is 0 and we fall through to Left.
    FlankSide best = FlankSide::Left;
    f32 best_dot = math::dot(side_offset_dir(fwd, FlankSide::Left), to_att);

    const f32 right_dot = math::dot(side_offset_dir(fwd, FlankSide::Right), to_att);
    if (right_dot > best_dot) {
        best_dot = right_dot;
        best = FlankSide::Right;
    }
    const f32 rear_dot = math::dot(side_offset_dir(fwd, FlankSide::Rear), to_att);
    if (rear_dot > best_dot) {
        best_dot = rear_dot;
        best = FlankSide::Rear;
    }
    return best;
}

bool in_front_arc(math::Vec3 target_pos, math::Vec3 target_facing,
                  math::Vec3 point, f32 front_cos_half_angle) noexcept {
    const math::Vec3 fwd = forward_xz(target_facing);

    const math::Vec3 to_pt_raw{point.x - target_pos.x, 0.0f,
                               point.z - target_pos.z};
    // A point on top of the target (XZ) has no direction => not in the arc.
    if (math::length(to_pt_raw) <= 0.0f) {
        return false;
    }
    const math::Vec3 dir = math::normalize(to_pt_raw);
    return math::dot(fwd, dir) >= front_cos_half_angle;
}

}  // namespace psynder::ai
