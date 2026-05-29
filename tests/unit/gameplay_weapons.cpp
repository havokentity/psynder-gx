// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_weapons.cpp — deterministic hitscan + projectile weapons:
// damage on hit, fire-rate gating, ammo, misses, projectile travel + impact +
// despawn, and cross-world determinism.

#include "gameplay/GameplayComponents.h"
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

TEST_CASE("gameplay: hitscan damages the target on the ray, gated by fire rate",
          "[gameplay][weapons]") {
    World w;
    const Entity shooter = spawn_shooter(w, 35.0f, 0.5f, 10);
    const Entity target = spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f);

    const Entity v = fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0});  // +X hits
    REQUIRE(v == target);
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(65.0f));
    REQUIRE(w.get<Weapon>(shooter)->ammo == 9);

    // Immediate refire is blocked by the cooldown.
    REQUIRE_FALSE(fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0}).valid());
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(65.0f));
    REQUIRE(w.get<Weapon>(shooter)->ammo == 9);  // not spent

    // After the cooldown elapses, it fires again.
    tick_weapons(w, 0.5f);
    REQUIRE(fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0}) == target);
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(30.0f));
}

TEST_CASE("gameplay: hitscan misses off-axis but still spends a round",
          "[gameplay][weapons]") {
    World w;
    const Entity shooter = spawn_shooter(w, 35.0f, 0.5f, 3);
    spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f);
    // Aim +Y; the target on +X is not on the ray.
    REQUIRE_FALSE(fire_hitscan(w, shooter, {0, 0, 0}, {0, 1, 0}).valid());
    REQUIRE(w.get<Weapon>(shooter)->ammo == 2);  // a miss still costs ammo
}

TEST_CASE("gameplay: an empty weapon cannot fire", "[gameplay][weapons]") {
    World w;
    const Entity shooter = spawn_shooter(w, 35.0f, 0.5f, 0);  // no ammo
    spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE_FALSE(fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0}).valid());
}

TEST_CASE("gameplay: a projectile flies, impacts a target, and despawns",
          "[gameplay][weapons]") {
    World w;
    const Entity shooter = spawn_shooter(w, 50.0f, 0.5f, 5);
    const Entity target = spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f);

    const Entity proj = fire_projectile(w, shooter, {0, 0, 0}, {1, 0, 0},
                                        /*speed=*/10.0f, /*ttl=*/5.0f);
    REQUIRE(proj.valid());
    REQUIRE(w.get<Projectile>(proj) != nullptr);

    std::vector<Entity> scratch;
    constexpr f32 dt = 1.0f / 120.0f;
    for (int i = 0; i < 80; ++i) tick_projectiles(w, dt, scratch);

    REQUIRE(w.get<Projectile>(proj) == nullptr);          // despawned on impact
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(50.0f));  // took 50
}

TEST_CASE("gameplay: a projectile with no target despawns on ttl",
          "[gameplay][weapons]") {
    World w;
    const Entity shooter = spawn_shooter(w, 50.0f, 0.5f, 5);
    const Entity proj = fire_projectile(w, shooter, {0, 0, 0}, {0, 1, 0},
                                        /*speed=*/10.0f, /*ttl=*/0.25f);
    REQUIRE(proj.valid());
    std::vector<Entity> scratch;
    constexpr f32 dt = 1.0f / 120.0f;
    for (int i = 0; i < 40; ++i) tick_projectiles(w, dt, scratch);  // > 0.25 s
    REQUIRE(w.get<Projectile>(proj) == nullptr);
}

TEST_CASE("gameplay: weapons fire is deterministic across worlds",
          "[gameplay][weapons][determinism]") {
    const auto run = []() {
        World w;
        const Entity shooter = spawn_shooter(w, 20.0f, 0.1f, -1);  // infinite ammo
        std::vector<Entity> tg;
        for (int i = 0; i < 4; ++i)
            tg.push_back(spawn_target(w, {5.0f, static_cast<f32>(i) * 2.0f, 0.0f},
                                      100.0f));
        std::vector<Entity> scratch;
        constexpr f32 dt = 1.0f / 120.0f;
        for (int step = 0; step < 120; ++step) {
            // Sweep the aim deterministically across the column of targets.
            const f32 y = static_cast<f32>(step % 4) * 2.0f;
            fire_hitscan(w, shooter, {0, 0, 0}, {5.0f, y, 0.0f});
            tick_weapons(w, dt);
            tick_projectiles(w, dt, scratch);
        }
        std::vector<f32> hp;
        for (Entity e : tg) hp.push_back(w.get<Health>(e) ? w.get<Health>(e)->hp : -1.0f);
        return hp;
    };
    REQUIRE(run() == run());
}
