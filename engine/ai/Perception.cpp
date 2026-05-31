// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Perception.cpp — see Perception.h.

#include "ai/Perception.h"

#include <cmath>  // std::sqrt, std::cos

namespace psynder::ai {

namespace {

constexpr f32 kEps = 1.0e-6f;

f32 dot3(math::Vec3 a, math::Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

}  // namespace

bool can_perceive(math::Vec3 observer_pos, math::Vec3 observer_facing,
                  math::Vec3 target_pos, f32 fov_cos_half_angle,
                  f32 range_m) noexcept {
    if (range_m < 0.0f) return false;
    const math::Vec3 to{target_pos.x - observer_pos.x,
                        target_pos.y - observer_pos.y,
                        target_pos.z - observer_pos.z};
    const f32 d2 = dot3(to, to);
    if (d2 > range_m * range_m) return false;  // out of range
    const f32 d = std::sqrt(d2);
    if (d <= kEps) return true;  // target at the observer

    const f32 fl = std::sqrt(dot3(observer_facing, observer_facing));
    if (fl <= kEps) return true;  // no facing -> omnidirectional within range

    // cos(angle between facing and the direction to the target).
    const f32 cosang = dot3(observer_facing, to) / (fl * d);
    return cosang >= fov_cos_half_angle;
}

f32 perception_strength(math::Vec3 observer_pos, math::Vec3 observer_facing,
                        math::Vec3 target_pos, f32 fov_cos_half_angle,
                        f32 range_m) noexcept {
    if (range_m <= 0.0f) return 0.0f;
    const math::Vec3 to{target_pos.x - observer_pos.x,
                        target_pos.y - observer_pos.y,
                        target_pos.z - observer_pos.z};
    const f32 d2 = dot3(to, to);
    if (d2 > range_m * range_m) return 0.0f;
    const f32 d = std::sqrt(d2);
    if (d <= kEps) return 1.0f;  // dead on top of the observer = full perception

    const f32 fl = std::sqrt(dot3(observer_facing, observer_facing));
    f32 angle_factor = 1.0f;  // omnidirectional default (no facing)
    if (fl > kEps) {
        const f32 cosang = dot3(observer_facing, to) / (fl * d);
        if (cosang < fov_cos_half_angle) return 0.0f;  // outside the cone
        const f32 denom = 1.0f - fov_cos_half_angle;
        angle_factor = (denom > kEps) ? (cosang - fov_cos_half_angle) / denom : 1.0f;
        if (angle_factor < 0.0f) angle_factor = 0.0f;
        if (angle_factor > 1.0f) angle_factor = 1.0f;
    }

    const f32 dist_factor = 1.0f - d / range_m;  // (0,1]
    f32 s = dist_factor * angle_factor;
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    return s;
}

f32 fov_cos(f32 half_angle_deg) noexcept {
    return std::cos(half_angle_deg * (math::kPi / 180.0f));
}

i32 most_perceptible(math::Vec3 observer_pos, math::Vec3 facing,
                     std::span<const math::Vec3> targets,
                     f32 fov_cos_half_angle, f32 range_m) noexcept {
    i32 best = -1;
    f32 best_s = 0.0f;
    for (usize i = 0; i < targets.size(); ++i) {
        const f32 s = perception_strength(observer_pos, facing, targets[i],
                                          fov_cos_half_angle, range_m);
        if (s > best_s) {  // strict > keeps the lowest index on a tie
            best_s = s;
            best = static_cast<i32>(i);
        }
    }
    return best;
}

}  // namespace psynder::ai
