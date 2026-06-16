// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Orbit.cpp — third-person orbit / chase camera.
// See Orbit.h for the public contract, the exact forward formula, and the
// determinism note (same-platform strict-FP; cosmetic view lane, not lockstep).

#include "camera/Orbit.h"

#include <cmath>  // std::sin, std::cos, std::floor, std::isfinite

namespace psynder::camera {
namespace {

// Degrees → radians. Kept local (the orbit lane stays self-contained and does
// not expand engine/math/'s surface), mirroring Camera.cpp's local helper.
constexpr f32 kPi        = 3.14159265358979323846f;
constexpr f32 kDegToRad  = kPi / 180.0f;

inline f32 deg_to_rad(f32 deg) noexcept {
    return deg * kDegToRad;
}

// Treat a non-finite value as 0 so a glitched delta can never enter the orbit
// state (matches Camera.cpp's input sanitisation strategy).
inline f32 finite_or_zero(f32 v) noexcept {
    return std::isfinite(v) ? v : 0.0f;
}

// NaN-safe clamp. On IEEE 754 both `v < lo` and `v > hi` are false for NaN, so
// the NaN guard returns `lo` rather than letting NaN leak into the state.
inline f32 clamp_finite(f32 v, f32 lo, f32 hi) noexcept {
    if (!(v == v)) return lo;  // NaN (NaN != NaN)
    if (v < lo)    return lo;
    if (v > hi)    return hi;
    return v;
}

// Wrap an angle into [-180, +180]. O(1) floor-based wrap (no while-loop, which a
// large mouse-X spike could stretch into a stall) — same form Camera.cpp uses.
// NaN guard returns 0 (neutral heading).
inline f32 wrap_deg_180(f32 v) noexcept {
    if (!(v == v)) return 0.0f;                       // NaN → neutral heading
    if (v >= -180.0f && v <= 180.0f) return v;        // fast path: in range
    return v - std::floor((v + 180.0f) / 360.0f) * 360.0f;
}

}  // namespace

OrbitResult orbit_eye(const OrbitState& s) noexcept {
    const f32 y = deg_to_rad(s.yaw_deg);
    const f32 p = deg_to_rad(s.pitch_deg);

    const f32 sy = std::sin(y);
    const f32 cy = std::cos(y);
    const f32 sp = std::sin(p);
    const f32 cp = std::cos(p);

    // Orbit pitch is the EYE ELEVATION (positive pitch raises the camera so it
    // looks DOWN at the target), the chase-cam convention. The look direction
    // (eye -> target) therefore tilts toward -Y for a positive pitch, so its Y
    // component is -sin(pitch) (with eye = target - forward*d that lifts the eye
    // above the target). yaw matches Camera.cpp's horizontal basis.
    //   forward = ( cos(pitch)*sin(yaw), -sin(pitch), -cos(pitch)*cos(yaw) )
    //   yaw=0,pitch=0 → (0,0,-1).
    const f32 fx =  cp * sy;
    const f32 fy = -sp;
    const f32 fz = -cp * cy;

    // The camera looks TOWARD the target along `forward`; the eye sits
    // `distance_m` behind the target along that same direction:
    //   eye = target - forward * distance_m
    // forward is already unit-length (it's a rotation of (0,0,-1)), so the eye
    // is exactly distance_m from the target.
    const f32 d = s.distance_m;

    OrbitResult r;
    r.eye[0] = s.target[0] - fx * d;
    r.eye[1] = s.target[1] - fy * d;
    r.eye[2] = s.target[2] - fz * d;

    r.forward[0] = fx;
    r.forward[1] = fy;
    r.forward[2] = fz;

    r.yaw_deg   = s.yaw_deg;
    r.pitch_deg = s.pitch_deg;
    return r;
}

void orbit_rotate(OrbitState& s, f32 yaw_delta_deg, f32 pitch_delta_deg) noexcept {
    // Yaw accumulates then wraps into [-180, 180]; pitch accumulates then clamps
    // into [-89, 89] (matching Camera.h's integration). Bad deltas → 0.
    s.yaw_deg   = wrap_deg_180(s.yaw_deg + finite_or_zero(yaw_delta_deg));
    s.pitch_deg = clamp_finite(s.pitch_deg + finite_or_zero(pitch_delta_deg),
                               -89.0f, 89.0f);
}

void orbit_zoom(OrbitState& s, f32 delta_m, f32 min_dist, f32 max_dist) noexcept {
    // Normalise a degenerate range (min > max) by swapping so the clamp still
    // lands inside the intended band rather than collapsing to the wrong bound.
    f32 lo = min_dist;
    f32 hi = max_dist;
    if (lo > hi) {
        const f32 t = lo;
        lo = hi;
        hi = t;
    }
    s.distance_m = clamp_finite(s.distance_m + finite_or_zero(delta_m), lo, hi);
}

}  // namespace psynder::camera
