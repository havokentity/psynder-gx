// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_melee.cpp — deterministic short-range cone melee:
// a front-arc hit damages + credits a kill, a target behind the swing is
// missed, a target beyond reach is missed, the nearest of several qualifiers
// is struck, the friendly-team filter passes over a teammate to the enemy
// behind, and the whole resolution is bit-identical across worlds.

#include "gameplay/GameplayComponents.h"
#include "gameplay/Melee.h"
#include "gameplay/Weapons.h"  // kNoTeam

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

Entity spawn_attacker(World& w) {
    const Entity e = w.create();
    w.add(e, Score{0u, 0u});  // so a kill can credit a frag
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

// A 90-degree half-angle cone (cos == 0): the full forward half-space.
constexpr f32 kHalfSpaceCos = 0.0f;

}  // namespace

TEST_CASE("gameplay: melee strikes a target in the front cone within range",
          "[gameplay][melee]") {
    World w;
    const Entity attacker = spawn_attacker(w);
    const Entity target = spawn_target(w, {2.0f, 0.0f, 0.0f}, 100.0f);

    // Swing +X, 3 m reach, forward half-space; the target at +2X is dead ahead.
    const Entity v = melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0},
                                  /*range_m=*/3.0f, kHalfSpaceCos,
                                  /*damage=*/40.0f);
    REQUIRE(v == target);
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(60.0f));  // dropped by 40
}

TEST_CASE("gameplay: a lethal melee swing credits the attacker a frag",
          "[gameplay][melee]") {
    World w;
    const Entity attacker = spawn_attacker(w);
    const Entity target = spawn_target(w, {1.5f, 0.0f, 0.0f}, 30.0f);

    const Entity v = melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0},
                                  /*range_m=*/3.0f, kHalfSpaceCos,
                                  /*damage=*/40.0f);
    REQUIRE(v == target);
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(0.0f));  // killed
    REQUIRE(w.get<Score>(attacker)->frags == 1u);               // frag credited
}

TEST_CASE("gameplay: melee does not hit a target behind the swing",
          "[gameplay][melee]") {
    World w;
    const Entity attacker = spawn_attacker(w);
    // Swing +X, but the target sits behind on -X: outside the forward cone.
    const Entity target = spawn_target(w, {-2.0f, 0.0f, 0.0f}, 100.0f);

    const Entity v = melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0},
                                  /*range_m=*/3.0f, kHalfSpaceCos,
                                  /*damage=*/40.0f);
    REQUIRE_FALSE(v.valid());
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(100.0f));  // untouched
}

TEST_CASE("gameplay: melee does not reach a target beyond range",
          "[gameplay][melee]") {
    World w;
    const Entity attacker = spawn_attacker(w);
    // Dead ahead and inside the cone, but 5 m out with only a 3 m reach.
    const Entity target = spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f);

    const Entity v = melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0},
                                  /*range_m=*/3.0f, kHalfSpaceCos,
                                  /*damage=*/40.0f);
    REQUIRE_FALSE(v.valid());
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(100.0f));  // untouched
}

TEST_CASE("gameplay: melee strikes the nearest of two valid targets",
          "[gameplay][melee]") {
    World w;
    const Entity attacker = spawn_attacker(w);
    const Entity nearer = spawn_target(w, {1.5f, 0.0f, 0.0f}, 100.0f);
    const Entity farther = spawn_target(w, {2.5f, 0.0f, 0.0f}, 100.0f);

    const Entity v = melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0},
                                  /*range_m=*/3.0f, kHalfSpaceCos,
                                  /*damage=*/40.0f);
    REQUIRE(v == nearer);
    REQUIRE(w.get<Health>(nearer)->hp == Catch::Approx(60.0f));   // hit
    REQUIRE(w.get<Health>(farther)->hp == Catch::Approx(100.0f)); // spared
}

TEST_CASE("gameplay: the friendly-team filter passes a mate and hits the enemy",
          "[gameplay][melee]") {
    World w;
    const Entity attacker = spawn_attacker(w);
    w.add(attacker, Team{0u});
    // A teammate stands nearer, directly ahead; an enemy is farther along.
    const Entity mate = spawn_target(w, {1.5f, 0.0f, 0.0f}, 100.0f);
    w.add(mate, Team{0u});
    const Entity enemy = spawn_target(w, {2.5f, 0.0f, 0.0f}, 100.0f);
    w.add(enemy, Team{1u});

    // Free-for-all (no team filter) strikes the NEAREST body — the teammate.
    const Entity ffa = melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0},
                                    /*range_m=*/3.0f, kHalfSpaceCos,
                                    /*damage=*/40.0f, kNoTeam);
    REQUIRE(ffa == mate);
    REQUIRE(w.get<Health>(mate)->hp == Catch::Approx(60.0f));
    REQUIRE(w.get<Health>(enemy)->hp == Catch::Approx(100.0f));

    // Reset; with the attacker's team as the friendly filter the swing passes
    // over the teammate and strikes the enemy behind them.
    w.get<Health>(mate)->hp = 100.0f;
    const Entity v = melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0},
                                  /*range_m=*/3.0f, kHalfSpaceCos,
                                  /*damage=*/40.0f, /*friendly_team=*/0);
    REQUIRE(v == enemy);
    REQUIRE(w.get<Health>(mate)->hp == Catch::Approx(100.0f));  // mate spared
    REQUIRE(w.get<Health>(enemy)->hp == Catch::Approx(60.0f));  // enemy hit
}

TEST_CASE("gameplay: a narrow cone excludes an off-axis target",
          "[gameplay][melee]") {
    World w;
    const Entity attacker = spawn_attacker(w);
    // Target at 45 degrees off the +X axis; a ~30-degree half-angle cone
    // (cos ~ 0.866) is too tight to include it.
    const Entity target = spawn_target(w, {2.0f, 2.0f, 0.0f}, 100.0f);
    const f32 narrow_cos = melee_cone_cos(30.0f);  // setup-time trig is fine

    const Entity v = melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0},
                                  /*range_m=*/5.0f, narrow_cos,
                                  /*damage=*/40.0f);
    REQUIRE_FALSE(v.valid());
    REQUIRE(w.get<Health>(target)->hp == Catch::Approx(100.0f));
}

TEST_CASE("gameplay: degenerate inputs hit nothing", "[gameplay][melee]") {
    World w;
    const Entity attacker = spawn_attacker(w);
    spawn_target(w, {2.0f, 0.0f, 0.0f}, 100.0f);

    // Zero-length facing axis: no cone direction.
    REQUIRE_FALSE(melee_attack(w, attacker, {0, 0, 0}, {0, 0, 0}, 3.0f,
                               kHalfSpaceCos, 40.0f)
                      .valid());
    // Non-positive reach: nothing is in range.
    REQUIRE_FALSE(melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0}, 0.0f,
                               kHalfSpaceCos, 40.0f)
                      .valid());
}

TEST_CASE("gameplay: melee resolution is deterministic across worlds",
          "[gameplay][melee][determinism]") {
    const auto run = []() {
        World w;
        const Entity attacker = spawn_attacker(w);
        std::vector<Entity> tg;
        for (int i = 0; i < 5; ++i)
            tg.push_back(spawn_target(
                w, {1.0f + static_cast<f32>(i) * 0.5f, 0.0f, 0.0f}, 100.0f));

        // Several swings; each chips the nearest LIVING target ahead, so the
        // victim selection (nearest, ascending-id ties) must replay identically.
        const Entity victims[6] = {
            melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0}, 5.0f, kHalfSpaceCos,
                         30.0f),
            melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0}, 5.0f, kHalfSpaceCos,
                         30.0f),
            melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0}, 5.0f, kHalfSpaceCos,
                         30.0f),
            melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0}, 5.0f, kHalfSpaceCos,
                         30.0f),
            melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0}, 5.0f, kHalfSpaceCos,
                         30.0f),
            melee_attack(w, attacker, {0, 0, 0}, {1, 0, 0}, 5.0f, kHalfSpaceCos,
                         30.0f),
        };

        std::vector<u64> out;
        for (const Entity v : victims) out.push_back(v.raw);
        for (const Entity e : tg)
            out.push_back(w.get<Health>(e)
                              ? static_cast<u64>(w.get<Health>(e)->hp)
                              : 0xFFFFFFFFull);
        return out;
    };
    REQUIRE(run() == run());
}
