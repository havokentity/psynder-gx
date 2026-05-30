// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainCollision.cpp — see TerrainCollision.h. Built on
// the deterministic HeightfieldQuery sampler (terrain_height / terrain_normal)
// so the sphere collides against exactly the data the renderer draws and the
// agents path over. Pure algebra + sqrt; no transcendentals; lockstep-safe.

#include "world/outdoor/TerrainCollision.h"

#include "world/outdoor/HeightfieldQuery.h"  // terrain_height, terrain_normal

namespace psynder::world::outdoor {

SphereHit sphere_vs_terrain(const HeightmapDesc& h, math::Vec3 center,
                            f32 radius_m) noexcept {
    SphereHit out;

    const f32 ground = terrain_height(h, center.x, center.z);
    // Penetration is how far the sphere's bottom (center.y - radius) sits below
    // the surface: (ground + radius) - center.y. Positive => into the ground.
    const f32 penetration = (ground + radius_m) - center.y;

    if (penetration > 0.0f) {
        out.penetrating = true;
        out.penetration_m = penetration;
        out.normal = terrain_normal(h, center.x, center.z);
        // Lift the centre straight up so the sphere's bottom rests on the
        // surface: center.y = terrain_height + radius.
        out.resolved_center = math::Vec3{center.x, ground + radius_m, center.z};
    } else {
        out.penetrating = false;
        out.penetration_m = 0.0f;
        // normal stays the default +Y; resolved centre is the input, unchanged.
        out.resolved_center = center;
    }
    return out;
}

math::Vec3 reflect_velocity(math::Vec3 velocity, math::Vec3 normal,
                            f32 restitution) noexcept {
    // v' = v - (1 + e) * dot(v, n) * n. With e == 0 the normal component is
    // removed (slide); with e == 1 it is mirrored (perfect bounce).
    const f32 vn = math::dot(velocity, normal);
    const f32 scale = (1.0f + restitution) * vn;
    return math::sub(velocity, math::mul(normal, scale));
}

bool resolve_sphere(const HeightmapDesc& h, math::Vec3& center,
                    math::Vec3& velocity, f32 radius_m,
                    f32 restitution) noexcept {
    const SphereHit hit = sphere_vs_terrain(h, center, radius_m);
    if (!hit.penetrating) return false;
    center = hit.resolved_center;
    velocity = reflect_velocity(velocity, hit.normal, restitution);
    return true;
}

}  // namespace psynder::world::outdoor
