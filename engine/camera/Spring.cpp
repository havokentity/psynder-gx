// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Spring.cpp — critically-damped smoothing spring ("SmoothDamp").
// See Spring.h for the algorithm, tuning, and determinism contract.

#include "camera/Spring.h"

#include <cmath>  // std::isfinite

namespace psynder::camera {

namespace {
// Lower bound on smooth_time so omega = 2 / smooth_time stays finite and the
// step can't divide by zero. A tiny positive floor keeps a zero / non-finite
// smooth_time from blowing up: it just makes the chase very snappy.
constexpr f32 kMinSmoothTime = 1.0e-4f;
}  // namespace

void spring_init(SpringState& s, f32 value) noexcept {
    s.value    = value;
    s.velocity = 0.0f;
}

f32 spring_update(SpringState& s, f32 target, f32 smooth_time_s,
                  f32 dt_s) noexcept {
    // A stalled / bogus frame clock leaves the spring untouched (no NaN leak).
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) return s.value;
    // A non-finite target can never enter the integrator (would poison value).
    if (!std::isfinite(target)) return s.value;

    // Clamp smooth_time UP to a tiny positive floor: a zero / tiny / non-finite
    // smooth_time can't divide-by-zero or push omega to infinity. (!(x >= floor)
    // also catches NaN, which fails the comparison.)
    f32 smooth_time = smooth_time_s;
    if (!(smooth_time >= kMinSmoothTime)) smooth_time = kMinSmoothTime;

    // Game Programming Gems 4 §1.10 critically-damped spring (Unity's
    // Mathf.SmoothDamp form). omega = 2 / smooth_time; the transcendental
    // e^(-x) is replaced by the polynomial reciprocal below so the whole step
    // is pure +,-,*,/ — deterministic on a given platform (see Spring.h).
    const f32 omega = 2.0f / smooth_time;

    const f32 x   = omega * dt_s;
    const f32 exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);

    const f32 change = s.value - target;             // signed distance to go
    const f32 temp   = (s.velocity + omega * change) * dt_s;

    s.velocity = (s.velocity - omega * temp) * exp;  // new rate
    s.value    = target + (change + temp) * exp;     // new value
    return s.value;
}

bool spring_settled(const SpringState& s, f32 target, f32 epsilon) noexcept {
    // Settled only when BOTH the value is within epsilon of the target AND the
    // rate is within epsilon of 0 (a value can pass through the target while
    // still moving fast — that is not settled).
    const f32 dv = s.value - target;
    const f32 dist = dv < 0.0f ? -dv : dv;               // |value - target|
    const f32 vel = s.velocity < 0.0f ? -s.velocity      // |velocity|
                                      : s.velocity;
    return dist <= epsilon && vel <= epsilon;
}

}  // namespace psynder::camera
