// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_splash.cpp — deterministic radial (blast) damage:
// full damage in the core, linear falloff to the outer radius, nothing beyond
// it, multi-victim order-independence, the friendly-team filter, rocket-jump
// self-damage, and cross-world determinism.

#include "gameplay/Damage.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/Splash.h"
#include "gameplay/Weapons.h"  // kNoTeam sentinel

#include "scene/GxComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

// An armor-free Health entity at `pos`, so its post-blast health delta is exactly
// the computed splash damage (no Quake armor ratio in the way).
Entity spawn_victim(World& w, math::Vec3 pos, f32 hp) {
    const Entity e = w.create();
    w.add(e, Health{hp, hp});
    TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    return e;
}

}  // namespace

TEST_CASE("gameplay: a victim at the blast centre takes full max damage",
          "[gameplay]") {
    World w;
    const Entity attacker = w.create();
    const Entity v = spawn_victim(w, {0.0f, 0.0f, 0.0f}, 100.0f);

    std::vector<Entity> scratch;
    const SplashParams p{2.0f, 10.0f, 80.0f};
    const usize hits = apply_splash_damage(w, {0, 0, 0}, p, attacker, kNoTeam,
                                           scratch);
    REQUIRE(hits == 1);
    REQUIRE(w.get<Health>(v)->hp == Catch::Approx(20.0f));  // 100 - 80
}

TEST_CASE("gameplay: a victim at the inner radius still takes full damage",
          "[gameplay]") {
    World w;
    const Entity attacker = w.create();
    // Exactly inner_radius_m away (2 m on +X) -> still the full-damage core.
    const Entity v = spawn_victim(w, {2.0f, 0.0f, 0.0f}, 100.0f);

    std::vector<Entity> scratch;
    const SplashParams p{2.0f, 10.0f, 80.0f};
    REQUIRE(apply_splash_damage(w, {0, 0, 0}, p, attacker, kNoTeam, scratch) == 1);
    REQUIRE(w.get<Health>(v)->hp == Catch::Approx(20.0f));  // full 80 absorbed
}

TEST_CASE("gameplay: a victim between inner and outer takes partial damage",
          "[gameplay]") {
    World w;
    const Entity attacker = w.create();
    // 6 m out, inner 2, outer 10 -> t = (6-2)/(10-2) = 0.5 -> 80 * 0.5 = 40.
    const Entity v = spawn_victim(w, {0.0f, 6.0f, 0.0f}, 100.0f);

    std::vector<Entity> scratch;
    const SplashParams p{2.0f, 10.0f, 80.0f};
    REQUIRE(apply_splash_damage(w, {0, 0, 0}, p, attacker, kNoTeam, scratch) == 1);
    const f32 hp = w.get<Health>(v)->hp;
    REQUIRE(hp == Catch::Approx(60.0f));  // 100 - 40
    REQUIRE(hp > 20.0f);   // strictly less than the full-damage result
    REQUIRE(hp < 100.0f);  // but strictly more than untouched
}

TEST_CASE("gameplay: a victim beyond the outer radius takes nothing",
          "[gameplay]") {
    World w;
    const Entity attacker = w.create();
    const Entity v = spawn_victim(w, {0.0f, 0.0f, 12.0f}, 100.0f);  // 12 m > 10 m

    std::vector<Entity> scratch;
    const SplashParams p{2.0f, 10.0f, 80.0f};
    REQUIRE(apply_splash_damage(w, {0, 0, 0}, p, attacker, kNoTeam, scratch) == 0);
    REQUIRE(w.get<Health>(v)->hp == Catch::Approx(100.0f));  // untouched
}

TEST_CASE("gameplay: multiple victims are all hit, order-independent",
          "[gameplay]") {
    World w;
    const Entity attacker = w.create();

    // Spawn in a SCRAMBLED spatial order, then assert each by its captured id so
    // the result cannot depend on spawn / iteration order.
    const Entity far = spawn_victim(w, {0.0f, 6.0f, 0.0f}, 100.0f);    // partial 40
    const Entity center = spawn_victim(w, {0.0f, 0.0f, 0.0f}, 100.0f);  // full 80
    const Entity inner = spawn_victim(w, {2.0f, 0.0f, 0.0f}, 100.0f);   // full 80
    const Entity out = spawn_victim(w, {0.0f, 0.0f, 12.0f}, 100.0f);    // none

    std::vector<Entity> scratch;
    const SplashParams p{2.0f, 10.0f, 80.0f};
    const usize hits = apply_splash_damage(w, {0, 0, 0}, p, attacker, kNoTeam,
                                           scratch);
    REQUIRE(hits == 3);  // three within range, the fourth spared
    REQUIRE(w.get<Health>(center)->hp == Catch::Approx(20.0f));
    REQUIRE(w.get<Health>(inner)->hp == Catch::Approx(20.0f));
    REQUIRE(w.get<Health>(far)->hp == Catch::Approx(60.0f));
    REQUIRE(w.get<Health>(out)->hp == Catch::Approx(100.0f));
}

TEST_CASE("gameplay: the friendly-team filter spares teammates but hits enemies",
          "[gameplay]") {
    World w;
    const Entity attacker = w.create();

    const Entity mate = spawn_victim(w, {1.0f, 0.0f, 0.0f}, 100.0f);
    w.add(mate, Team{0u});
    const Entity enemy = spawn_victim(w, {1.0f, 0.0f, 0.0f}, 100.0f);
    w.add(enemy, Team{1u});

    std::vector<Entity> scratch;
    const SplashParams p{2.0f, 10.0f, 80.0f};
    // Friendly team 0: spare the teammate, still blast the enemy.
    const usize hits = apply_splash_damage(w, {0, 0, 0}, p, attacker,
                                           /*friendly_team=*/0, scratch);
    REQUIRE(hits == 1);
    REQUIRE(w.get<Health>(mate)->hp == Catch::Approx(100.0f));   // spared
    REQUIRE(w.get<Health>(enemy)->hp == Catch::Approx(20.0f));   // full 80
}

TEST_CASE("gameplay: the attacker is damaged by its own blast (rocket-jump)",
          "[gameplay]") {
    World w;
    // The attacker is itself a Health entity, standing inside the blast.
    const Entity attacker = spawn_victim(w, {1.0f, 0.0f, 0.0f}, 100.0f);

    std::vector<Entity> scratch;
    const SplashParams p{2.0f, 10.0f, 80.0f};
    const usize hits = apply_splash_damage(w, {0, 0, 0}, p, attacker, kNoTeam,
                                           scratch);
    REQUIRE(hits == 1);
    REQUIRE(w.get<Health>(attacker)->hp == Catch::Approx(20.0f));  // self-hurt
}

TEST_CASE("gameplay: splash damage is deterministic across worlds",
          "[gameplay][determinism]") {
    const auto run = []() {
        World w;
        const Entity attacker = w.create();
        std::vector<Entity> victims;
        // A spread of victims at varied 3D offsets, spawned in mixed order.
        for (int i = 0; i < 6; ++i) {
            const f32 f = static_cast<f32>(i);
            victims.push_back(spawn_victim(
                w, {f, f * 0.5f, -f * 0.25f}, 100.0f));
        }
        std::vector<Entity> scratch;
        const SplashParams p{1.5f, 8.0f, 70.0f};
        // Several detonations at different points (no RNG anywhere).
        apply_splash_damage(w, {0, 0, 0}, p, attacker, kNoTeam, scratch);
        apply_splash_damage(w, {3, 0, 0}, p, attacker, kNoTeam, scratch);
        apply_splash_damage(w, {1, 1, -1}, p, attacker, kNoTeam, scratch);

        std::vector<f32> hp;
        for (Entity e : victims) hp.push_back(w.get<Health>(e)->hp);
        return hp;
    };
    const std::vector<f32> a = run();
    const std::vector<f32> b = run();
    REQUIRE(a == b);  // bit-identical health across two independent worlds
}
