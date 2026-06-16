// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/weapon_spread_integration.cpp — wiring of the deterministic cone
// spread (gameplay/Spread.h) into fire_hitscan: backward-compatible perfect
// accuracy by default, seed-driven scatter when spread_tan > 0, and
// cross-world determinism for a fixed (shooter, dir, spread_tan, seed).

#include "gameplay/GameplayComponents.h"
#include "gameplay/Spread.h"
#include "gameplay/Weapons.h"

#include "scene/GxComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

Entity spawn_shooter(World& w, f32 damage, f32 interval, i32 ammo) {
    const Entity e = w.create();
    w.add(e, Weapon{damage, interval, 0.0f, ammo, 0.5f});
    return e;
}
Entity spawn_target(World& w, math::Vec3 pos, f32 hp) {
    const Entity e = w.create();
    w.add(e, Health{hp, hp});
    TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    return e;
}

}  // namespace

TEST_CASE("weapon-spread: zero spread (default) still hits the on-axis target",
          "[gameplay][weapons]") {
    World w;
    const Entity shooter = spawn_shooter(w, 35.0f, 0.5f, 10);
    const Entity target = spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f);

    // Default spread_tan = 0 => perfect accuracy: identical to the legacy call.
    const Entity v = fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0});
    REQUIRE(v == target);
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(65.0f));

    // Explicitly passing spread_tan = 0 (with a seed) must behave the same.
    w.get<Weapon>(shooter)->cooldown_s = 0.0f;
    const Entity v2 = fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0},
                                   /*friendly_team=*/kNoTeam,
                                   /*spread_tan=*/0.0f, /*spread_seed=*/12345u);
    REQUIRE(v2 == target);
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(30.0f));
}

TEST_CASE("weapon-spread: positive spread scatters the fire direction by seed",
          "[gameplay][weapons]") {
    const math::Vec3 base = math::normalize({1.0f, 0.0f, 0.0f});
    const f32 spread_tan = 0.5f;  // ~26.6-degree half-angle cone

    // No spread is deterministic and leaves the base untouched.
    const math::Vec3 none = spread_direction(base, 0.0f, 7u);
    REQUIRE(none.x == Catch::Approx(base.x));
    REQUIRE(none.y == Catch::Approx(base.y));
    REQUIRE(none.z == Catch::Approx(base.z));

    // Different seeds perturb the direction differently (the cone scatters).
    const math::Vec3 a = spread_direction(base, spread_tan, spread_seed(0, 1, 0));
    const math::Vec3 b = spread_direction(base, spread_tan, spread_seed(0, 1, 1));
    const math::Vec3 c = spread_direction(base, spread_tan, spread_seed(0, 1, 2));

    const bool ab_differs = a.x != Catch::Approx(b.x) || a.y != Catch::Approx(b.y) ||
                            a.z != Catch::Approx(b.z);
    const bool ac_differs = a.x != Catch::Approx(c.x) || a.y != Catch::Approx(c.y) ||
                            a.z != Catch::Approx(c.z);
    REQUIRE((ab_differs || ac_differs));

    // Each perturbed direction stays a unit vector and inside the cone:
    //   dot(base, result) >= 1/sqrt(1 + spread_tan^2) == cos(half_angle).
    const f32 cos_half = 1.0f / std::sqrt(1.0f + spread_tan * spread_tan);
    for (const math::Vec3& r : {a, b, c}) {
        REQUIRE(math::length(r) == Catch::Approx(1.0f).margin(1e-4f));
        REQUIRE(math::dot(base, r) >= Catch::Approx(cos_half).margin(1e-4f));
    }

    // Drive it through fire_hitscan: a perfect (no-spread) shot at an on-axis
    // target always connects, but a WIDE cone scatters some shots off it — so
    // across seeds we observe at least one miss (spread has a real effect on the
    // ray) AND at least one hit. (Deterministic + robust: a 63 deg cone vs a
    // 0.5 m target 3 m away misses on most seeds.)
    const math::Vec3 origin{0, 0, 0};
    const math::Vec3 aim{1, 0, 0};

    {
        World w;
        const Entity shooter = spawn_shooter(w, 35.0f, 0.5f, -1);
        spawn_target(w, {3.0f, 0.0f, 0.0f}, 100.0f);  // on the +X ray
        REQUIRE(fire_hitscan(w, shooter, origin, aim).valid());  // perfect hit
    }

    bool any_missed = false, any_hit = false;
    for (u32 shot = 0; shot < 128; ++shot) {
        World w;
        const Entity shooter = spawn_shooter(w, 35.0f, 0.5f, -1);
        spawn_target(w, {3.0f, 0.0f, 0.0f}, 100.0f);
        const Entity v =
            fire_hitscan(w, shooter, origin, aim, /*friendly_team=*/kNoTeam,
                         /*spread_tan=*/2.0f, /*spread_seed=*/spread_seed(0, 1, shot));
        if (v.valid()) any_hit = true; else any_missed = true;
    }
    REQUIRE(any_missed);  // the cone scattered some shots off the target
    REQUIRE(any_hit);     // ...but the on-axis target is still hit on some
}

TEST_CASE("weapon-spread: same (shooter,dir,spread_tan,seed) is identical "
          "across two worlds",
          "[gameplay][weapons][determinism]") {
    const u64 seed = spread_seed(42, 7, 3);
    const f32 spread_tan = 0.75f;
    const math::Vec3 origin{0, 0, 0};
    const math::Vec3 aim{1, 0, 0};

    const auto run = [&]() {
        World w;
        const Entity shooter = spawn_shooter(w, 25.0f, 0.5f, -1);
        // A small fan of targets around the +X axis so the perturbed ray's
        // outcome (which one, if any, it strikes) depends on the seed.
        std::vector<Entity> tg;
        tg.push_back(spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f));
        tg.push_back(spawn_target(w, {5.0f, 1.5f, 0.0f}, 100.0f));
        tg.push_back(spawn_target(w, {5.0f, -1.5f, 0.0f}, 100.0f));
        tg.push_back(spawn_target(w, {5.0f, 0.0f, 1.5f}, 100.0f));

        const Entity victim = fire_hitscan(w, shooter, origin, aim,
                                           /*friendly_team=*/kNoTeam, spread_tan, seed);
        std::vector<f32> hp;
        for (Entity e : tg) hp.push_back(w.get<Health>(e)->hp);
        return std::pair<u64, std::vector<f32>>{victim.raw, std::move(hp)};
    };

    REQUIRE(run() == run());
}
