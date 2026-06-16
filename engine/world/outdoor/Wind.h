// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/Wind.h — a deterministic, smoothly-varying wind vector
// field over world position and time, sampled by COSMETIC systems (foliage
// drift, particle/cloth advection, flag/grass sway). It is a base direction +
// strength plus a gentle gust oscillation.
//
// Determinism / scope: wind_at uses transcendentals (sin/cos) for the gust
// term. Like HeightfieldQuery::generate_hills, libm sin/cos is bit-identical
// for the SAME platform + same args but is NOT guaranteed identical across
// arm64 / x86_64 / MSVC. This field is therefore PURELY COSMETIC and MUST NOT
// feed the authoritative lockstep simulation (no agent steering, no physics,
// no hit reg) — it would desync clients. Within those bounds it is pure: no
// RNG, no wall-clock, same (pos, time) => same vector. Metric: world X/Z/Y in
// metres, time in seconds, returned vector in metres/second.

#pragma once

#include "math/Math.h"
#include "core/Types.h"

namespace psynder::world::outdoor {

// Parameters of the wind field.
//  - base_dir:        nominal wind heading; should be ~unit and horizontal
//                     (small/zero y). It is normalized internally before use.
//  - base_strength:   steady wind speed along base_dir (m/s).
//  - gust_strength:   peak magnitude of the oscillating gust component (m/s),
//                     added PERPENDICULAR to base_dir in the horizontal plane
//                     so gusts buffet the steady flow side-to-side.
//  - gust_frequency_hz: temporal oscillation rate of the gust (cycles/second).
//  - spatial_scale:   how fast the gust phase varies across space (radians per
//                     metre); 0 makes the gust spatially uniform (time-only).
struct WindParams {
    math::Vec3 base_dir;
    f32        base_strength;
    f32        gust_strength;
    f32        gust_frequency_hz;
    f32        spatial_scale;
};

// A calm default breeze: a gentle +X wind with a mild side-to-side gust.
// Vec3 in Math.h is a trivial {f32 x,y,z} aggregate (a literal type), so a
// constexpr WindParams is well-formed.
inline constexpr WindParams kDefaultWind{
    /*base_dir*/        math::Vec3{1.0f, 0.0f, 0.0f},
    /*base_strength*/   3.0f,    // ~light breeze
    /*gust_strength*/   1.5f,
    /*gust_frequency_hz*/ 0.2f,  // one gust cycle every 5 s
    /*spatial_scale*/   0.05f,   // gust phase rolls ~one radian per 20 m
};

// Sample the wind vector (m/s) at a world position and time.
//
// Model:
//   dir   = normalize(base_dir)                       (steady heading)
//   perp  = horizontal vector perpendicular to dir    (gust buffeting axis)
//   phase = time * 2pi * gust_frequency_hz
//           + (world_pos.x + world_pos.z) * spatial_scale
//   wind  = dir * base_strength + perp * (gust_strength * sin(phase))
//
// The gust oscillates with both time and the (x+z) spatial phase, so the field
// drifts smoothly and is non-uniform across space. The result is horizontal
// (y ~ 0 when base_dir is horizontal). Deterministic same-platform (cosmetic).
math::Vec3 wind_at(const WindParams& p, math::Vec3 world_pos,
                   f32 time_s) noexcept;

// Magnitude (m/s) of wind_at(...) — the instantaneous wind speed at a sample.
f32 wind_strength_at(const WindParams& p, math::Vec3 world_pos,
                     f32 time_s) noexcept;

// A small positional sway offset (metres) for bending a grass blade / flag /
// foliage card: wind_at(...) * sway_amount. The offset magnitude is bounded by
// sway_amount * (base_strength + gust_strength) (the wind's peak speed), so
// callers get a predictable maximum displacement. Cosmetic; do not integrate
// into authoritative motion.
math::Vec3 wind_displacement(const WindParams& p, math::Vec3 world_pos,
                             f32 time_s, f32 sway_amount) noexcept;

}  // namespace psynder::world::outdoor
