// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/HeightfieldQuery.cpp — see HeightfieldQuery.h. Built on
// the shared inline sampler in Heightmap_internal.h so gameplay collides against
// exactly the data the renderer draws.

#include "world/outdoor/HeightfieldQuery.h"

#include "world/outdoor/Heightmap_internal.h"  // detail::sample_bilinear

#include <cmath>

namespace psynder::world::outdoor {

f32 terrain_height(const HeightmapDesc& h, f32 wx, f32 wz) noexcept {
    return detail::sample_bilinear(h, wx, wz);
}

math::Vec3 terrain_normal(const HeightmapDesc& h, f32 wx, f32 wz) noexcept {
    // Central differences of the bilinear field. e = one texel keeps the slope
    // smooth and matches the mesh tessellation. Spacing scales the gradient.
    const f32 e = h.spacing > 0.0f ? h.spacing : 1.0f;
    const f32 hl = detail::sample_bilinear(h, wx - e, wz);
    const f32 hr = detail::sample_bilinear(h, wx + e, wz);
    const f32 hd = detail::sample_bilinear(h, wx, wz - e);
    const f32 hu = detail::sample_bilinear(h, wx, wz + e);
    math::Vec3 n{(hl - hr), 2.0f * e, (hd - hu)};
    return math::normalize(n);
}

RayHit terrain_raycast(const HeightmapDesc& h, math::Vec3 origin, math::Vec3 dir,
                       f32 max_t, f32 step) noexcept {
    RayHit out;
    const f32 dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl <= 0.0f || max_t <= 0.0f) return out;
    const math::Vec3 d{dir.x / dl, dir.y / dl, dir.z / dl};

    if (step <= 0.0f) {
        const f32 sp = h.spacing > 0.0f ? h.spacing : 1.0f;
        step = sp * 0.5f;
        if (step < 0.05f) step = 0.05f;
    }

    // Signed height above the terrain: > 0 above the surface, <= 0 on/below it.
    auto above = [&](const math::Vec3& p) -> f32 {
        return p.y - detail::sample_bilinear(h, p.x, p.z);
    };

    f32 prev_t = 0.0f;
    f32 prev_a = above(origin);
    if (prev_a <= 0.0f) {  // started on/under the surface — immediate hit
        out.hit = true;
        out.t = 0.0f;
        out.point = origin;
        out.normal = terrain_normal(h, origin.x, origin.z);
        return out;
    }

    for (f32 t = step; t <= max_t; t += step) {
        const math::Vec3 p{origin.x + d.x * t, origin.y + d.y * t,
                           origin.z + d.z * t};
        const f32 a = above(p);
        if (a <= 0.0f) {
            // Crossing between prev_t (above) and t (on/below): bisect.
            f32 lo = prev_t, hi = t;
            for (int k = 0; k < 24; ++k) {
                const f32 mid = (lo + hi) * 0.5f;
                const math::Vec3 pm{origin.x + d.x * mid, origin.y + d.y * mid,
                                    origin.z + d.z * mid};
                if (above(pm) > 0.0f) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            out.hit = true;
            out.t = hi;
            out.point = math::Vec3{origin.x + d.x * hi, origin.y + d.y * hi,
                                   origin.z + d.z * hi};
            out.normal = terrain_normal(h, out.point.x, out.point.z);
            return out;
        }
        prev_t = t;
        prev_a = a;
    }
    return out;  // no crossing within max_t
}

math::Vec3 clamp_to_ground(const HeightmapDesc& h, math::Vec3 pos,
                           f32 foot_offset) noexcept {
    pos.y = detail::sample_bilinear(h, pos.x, pos.z) + foot_offset;
    return pos;
}

void generate_hills(std::vector<u16>& out, u32 size_x, u32 size_z, u32 seed,
                    f32 amplitude_units) noexcept {
    out.assign(static_cast<usize>(size_x) * size_z, 0u);
    const f32 phase = static_cast<f32>(seed) * 0.137f;
    for (u32 z = 0; z < size_z; ++z) {
        for (u32 x = 0; x < size_x; ++x) {
            const f32 fx = static_cast<f32>(x);
            const f32 fz = static_cast<f32>(z);
            // Sum of a few sines -> rolling hills, normalized to [0,1].
            f32 v = 0.5f + 0.25f * std::sin(fx * 0.05f + phase) +
                    0.15f * std::sin(fz * 0.07f + phase * 1.3f) +
                    0.10f * std::sin((fx + fz) * 0.031f + phase * 0.7f);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            out[static_cast<usize>(z) * size_x + x] =
                static_cast<u16>(v * amplitude_units);
        }
    }
}

}  // namespace psynder::world::outdoor
