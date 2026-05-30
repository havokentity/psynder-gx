// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/terrain_collision.cpp — deterministic sphere-vs-heightfield
// collision resolve (engine/world/outdoor/TerrainCollision): detect a sphere
// penetrating below the surface, push it out along the surface normal so its
// bottom rests on the ground, and optionally reflect velocity for a bounce.
// Lockstep-safe pure algebra, so it can run on the deterministic tick.

#include "world/outdoor/TerrainCollision.h"
#include "world/outdoor/Terrain.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <vector>

using namespace psynder;
using namespace psynder::world::outdoor;

namespace {
HeightmapDesc make_desc(const std::vector<u16>& heights, u32 sx, u32 sz,
                        f32 spacing, f32 scale) {
    HeightmapDesc d{};
    d.size_x = sx;
    d.size_z = sz;
    d.spacing = spacing;
    d.height_scale = scale;
    d.heights = heights.data();
    return d;
}
}  // namespace

TEST_CASE("terrain-collision: a sphere well above flat ground does not penetrate",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // 500 * 0.01 = 5 m everywhere
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    // Centre at y = 20, radius 1 -> bottom at 19, well above the 5 m surface.
    const SphereHit hit = sphere_vs_terrain(d, {3.0f, 20.0f, 3.0f}, 1.0f);
    REQUIRE_FALSE(hit.penetrating);
    REQUIRE(hit.penetration_m == Catch::Approx(0.0f));
    // resolved_center is the input, untouched.
    REQUIRE(hit.resolved_center.x == Catch::Approx(3.0f));
    REQUIRE(hit.resolved_center.y == Catch::Approx(20.0f));
    REQUIRE(hit.resolved_center.z == Catch::Approx(3.0f));
}

TEST_CASE("terrain-collision: a sphere into flat ground penetrates and seats on it",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // flat 5 m
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    // Centre at y = 5.4, radius 1 -> bottom at 4.4, which is 0.6 m below the
    // 5 m surface. Penetration = (5 + 1) - 5.4 = 0.6.
    const SphereHit hit = sphere_vs_terrain(d, {3.0f, 5.4f, 3.0f}, 1.0f);
    REQUIRE(hit.penetrating);
    REQUIRE(hit.penetration_m == Catch::Approx(0.6f));

    // Resolved centre lifts to terrain_height + radius = 6, so the bottom
    // (center.y - radius) sits exactly on the 5 m surface.
    REQUIRE(hit.resolved_center.y == Catch::Approx(6.0f));
    REQUIRE((hit.resolved_center.y - 1.0f) == Catch::Approx(5.0f));
    // XZ are unchanged by the vertical push-out.
    REQUIRE(hit.resolved_center.x == Catch::Approx(3.0f));
    REQUIRE(hit.resolved_center.z == Catch::Approx(3.0f));
}

TEST_CASE("terrain-collision: the surface normal on flat ground points up",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // flat 5 m
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const SphereHit hit = sphere_vs_terrain(d, {3.0f, 5.4f, 3.0f}, 1.0f);
    REQUIRE(hit.penetrating);
    REQUIRE(hit.normal.x == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(hit.normal.y == Catch::Approx(1.0f).margin(1e-5f));
    REQUIRE(hit.normal.z == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("terrain-collision: reflect_velocity bounces and stops by restitution",
          "[terrain][gameplay]") {
    const math::Vec3 up{0.0f, 1.0f, 0.0f};
    const math::Vec3 down{0.0f, -3.0f, 0.0f};

    // Perfect bounce: a downward velocity mirrors to the same speed upward.
    const math::Vec3 bounce = reflect_velocity(down, up, 1.0f);
    REQUIRE(bounce.x == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(bounce.y == Catch::Approx(3.0f));
    REQUIRE(bounce.z == Catch::Approx(0.0f).margin(1e-5f));

    // Zero restitution: the normal component is removed (the slide / stop case).
    const math::Vec3 slide = reflect_velocity(down, up, 0.0f);
    REQUIRE(slide.y == Catch::Approx(0.0f).margin(1e-5f));

    // The tangential component is preserved (a ball skimming the surface keeps
    // its horizontal speed; restitution only touches the normal direction).
    const math::Vec3 mixed{2.0f, -3.0f, 0.0f};
    const math::Vec3 r = reflect_velocity(mixed, up, 1.0f);
    REQUIRE(r.x == Catch::Approx(2.0f));
    REQUIRE(r.y == Catch::Approx(3.0f));
}

TEST_CASE("terrain-collision: resolve_sphere lifts and bounces a penetrating sphere",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // flat 5 m
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    math::Vec3 center{3.0f, 5.4f, 3.0f};       // bottom 0.6 m into the ground
    math::Vec3 velocity{0.0f, -4.0f, 0.0f};    // falling
    const bool resolved = resolve_sphere(d, center, velocity, 1.0f, 1.0f);

    REQUIRE(resolved);
    // Lifted so the bottom rests on the surface.
    REQUIRE(center.y == Catch::Approx(6.0f));
    // Reflected upward with restitution 1 (flat-ground normal is +Y).
    REQUIRE(velocity.y == Catch::Approx(4.0f));
}

TEST_CASE("terrain-collision: resolve_sphere no-ops a floating sphere",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // flat 5 m
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    math::Vec3 center{3.0f, 20.0f, 3.0f};      // well above the surface
    math::Vec3 velocity{1.0f, -2.0f, 0.5f};
    const math::Vec3 center0 = center;
    const math::Vec3 velocity0 = velocity;

    const bool resolved = resolve_sphere(d, center, velocity, 1.0f, 1.0f);
    REQUIRE_FALSE(resolved);
    // Both left untouched.
    REQUIRE(center.x == Catch::Approx(center0.x));
    REQUIRE(center.y == Catch::Approx(center0.y));
    REQUIRE(center.z == Catch::Approx(center0.z));
    REQUIRE(velocity.x == Catch::Approx(velocity0.x));
    REQUIRE(velocity.y == Catch::Approx(velocity0.y));
    REQUIRE(velocity.z == Catch::Approx(velocity0.z));
}

TEST_CASE("terrain-collision: resolve is bit-identical for repeated calls",
          "[terrain][gameplay][determinism]") {
    // A +X ramp so the normal is genuinely tilted (exercises the sqrt path).
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 100u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    // Centre below the ramp surface at x = 4 (terrain height ~ 4 m).
    const SphereHit a = sphere_vs_terrain(d, {4.0f, 4.2f, 4.0f}, 1.0f);
    const SphereHit b = sphere_vs_terrain(d, {4.0f, 4.2f, 4.0f}, 1.0f);
    REQUIRE(a.penetrating == b.penetrating);
    REQUIRE(a.penetration_m == b.penetration_m);          // exact
    REQUIRE(a.normal.x == b.normal.x);
    REQUIRE(a.normal.y == b.normal.y);
    REQUIRE(a.normal.z == b.normal.z);
    REQUIRE(a.resolved_center.y == b.resolved_center.y);  // exact

    // reflect_velocity is pure algebra: identical inputs, identical bits.
    const math::Vec3 v{1.0f, -5.0f, 2.0f};
    const math::Vec3 r1 = reflect_velocity(v, a.normal, 0.5f);
    const math::Vec3 r2 = reflect_velocity(v, a.normal, 0.5f);
    REQUIRE(r1.x == r2.x);
    REQUIRE(r1.y == r2.y);
    REQUIRE(r1.z == r2.z);
}
