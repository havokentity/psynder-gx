// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Steering.cpp — see Steering.h.
//
// All math is done on the XZ projection (y dropped on read, forced to 0 on the
// result). Every magnitude / normalize routes through one guarded sqrt: when the
// squared length is <= 0 we return the zero vector instead of dividing by zero,
// which keeps the output NaN-free and bit-deterministic for lockstep.

#include "ai/Steering.h"

#include <cmath>

namespace psynder::ai {

namespace {

// XZ unit vector from `from` toward `to`, scaled by `speed`. Returns the zero
// vector when the two points coincide on the XZ plane (guarded normalize).
math::Vec3 desired_toward(math::Vec3 from, math::Vec3 to, f32 speed) noexcept {
    const f32 dx = to.x - from.x;
    const f32 dz = to.z - from.z;
    const f32 len2 = dx * dx + dz * dz;
    if (len2 <= 0.0f) return math::Vec3{0.0f, 0.0f, 0.0f};  // coincident — no dir
    const f32 inv = speed / std::sqrt(len2);  // one guarded sqrt
    return math::Vec3{dx * inv, 0.0f, dz * inv};
}

}  // namespace

math::Vec3 seek(math::Vec3 pos, math::Vec3 target, f32 max_speed) noexcept {
    return desired_toward(pos, target, max_speed);
}

math::Vec3 flee(math::Vec3 pos, math::Vec3 threat, f32 max_speed) noexcept {
    // Away from the threat == toward (pos + (pos - threat)), i.e. point the
    // desired-toward helper from the threat to the agent.
    return desired_toward(threat, pos, max_speed);
}

math::Vec3 arrive(math::Vec3 pos, math::Vec3 target, f32 max_speed,
                  f32 slow_radius_m) noexcept {
    const f32 dx = target.x - pos.x;
    const f32 dz = target.z - pos.z;
    const f32 len2 = dx * dx + dz * dz;

    // Tiny arrival epsilon: within ~1 mm (1e-3 m, squared) of the target there
    // is no meaningful direction — ease to a full stop.
    constexpr f32 kArriveEps2 = 1.0e-6f;  // (1e-3 m)^2
    if (len2 <= kArriveEps2) return math::Vec3{0.0f, 0.0f, 0.0f};

    const f32 dist = std::sqrt(len2);  // one guarded sqrt (len2 > eps > 0)

    // Outside the slow radius: full speed. Inside: ramp linearly with distance,
    // clamped to [0, max_speed]. A non-positive slow radius degenerates to a hard
    // stop at the target (the eps check above already handles being on it).
    f32 speed = max_speed;
    if (slow_radius_m > 0.0f && dist < slow_radius_m) {
        speed = max_speed * (dist / slow_radius_m);
        if (speed < 0.0f)       speed = 0.0f;
        else if (speed > max_speed) speed = max_speed;
    }

    const f32 inv = speed / dist;
    return math::Vec3{dx * inv, 0.0f, dz * inv};
}

math::Vec3 clamp_speed(math::Vec3 velocity, f32 max_speed) noexcept {
    const f32 vx = velocity.x;
    const f32 vz = velocity.z;
    const f32 len2 = vx * vx + vz * vz;
    const f32 cap2 = max_speed * max_speed;

    // Under (or at) the cap, or stationary: keep it as-is (y forced to 0). Only
    // pay for the sqrt when we actually have to scale down.
    if (len2 <= cap2 || len2 <= 0.0f)
        return math::Vec3{vx, 0.0f, vz};

    const f32 scale = max_speed / std::sqrt(len2);  // one guarded sqrt
    return math::Vec3{vx * scale, 0.0f, vz * scale};
}

math::Vec3 blend(math::Vec3 a, math::Vec3 b, f32 wa, f32 wb) noexcept {
    return math::Vec3{a.x * wa + b.x * wb, 0.0f, a.z * wa + b.z * wb};
}

}  // namespace psynder::ai
