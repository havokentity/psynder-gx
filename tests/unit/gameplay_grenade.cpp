// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_grenade.cpp — a thrown, fuse-timed grenade that detonates
// into a radial splash blast: it arcs under gravity, holds its damage until the
// fuse expires, then detonates (full damage in the core, none beyond the outer
// radius), despawns the spent grenade, honours the friendly-team filter, rejects
// a zero throw direction, and is bit-identical across worlds.

#include "gameplay/Grenade.h"
#include "gameplay/Splash.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/Damage.h"
#include "gameplay/Weapons.h"  // kNoTeam sentinel

#include "scene/GxComponents.h"
#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

// World position of an entity from its TransformWS translation column.
math::Vec3 pos_of(World& w, Entity e) {
    const TransformWS* t = w.get<TransformWS>(e);
    return {t->mtw.m[12], t->mtw.m[13], t->mtw.m[14]};
}

// An armor-free Health entity at `pos`, so its post-blast health delta is
// exactly the computed splash damage (no Quake armor ratio in the way).
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

TEST_CASE("gameplay: a thrown grenade arcs under gravity", "[gameplay]") {
    World w;
    const Entity thrower = w.create();
    // Throw up-and-forward (+X, +Y). With this dt the upward velocity is large
    // enough that Y rises for the first ticks before gravity wins.
    const SplashParams blast{2.0f, 10.0f, 80.0f};
    const Entity g = throw_grenade(w, thrower, kNoTeam, {0.0f, 0.0f, 0.0f},
                                   {1.0f, 1.0f, 0.0f}, /*speed=*/14.142136f,
                                   /*fuse=*/100.0f, blast);
    REQUIRE(g.valid());

    std::vector<Entity> scratch;
    constexpr f32 dt = 0.05f;

    const math::Vec3 p0 = pos_of(w, g);
    tick_grenades(w, dt, scratch);
    const math::Vec3 p1 = pos_of(w, g);
    // After one tick: X advanced and Y rose (still ascending under the throw).
    REQUIRE(p1.x > p0.x);
    REQUIRE(p1.y > p0.y);

    // Keep ticking; X keeps advancing monotonically and Y eventually turns over
    // and falls below the launch height as gravity dominates.
    f32 peak_y = p1.y;
    f32 prev_x = p1.x;
    for (int i = 0; i < 80; ++i) {
        tick_grenades(w, dt, scratch);
        const math::Vec3 p = pos_of(w, g);
        REQUIRE(p.x > prev_x);  // forward velocity is constant -> X strictly up
        prev_x = p.x;
        if (p.y > peak_y) peak_y = p.y;
    }
    const math::Vec3 pend = pos_of(w, g);
    REQUIRE(peak_y > p0.y);        // the arc rose above the launch point
    REQUIRE(pend.y < p0.y);        // then fell back below it
    REQUIRE(w.get<Grenade>(g) != nullptr);  // long fuse -> still in flight
}

TEST_CASE("gameplay: the fuse counts down and does not damage before expiry",
          "[gameplay]") {
    World w;
    const Entity thrower = w.create();
    // A victim sitting right at the launch point, inside the eventual blast.
    const Entity victim = spawn_victim(w, {0.0f, 0.0f, 0.0f}, 100.0f);

    const SplashParams blast{2.0f, 10.0f, 80.0f};
    // Throw nearly straight up so the grenade stays near the victim; short fuse.
    const Entity g = throw_grenade(w, thrower, kNoTeam, {0.0f, 0.5f, 0.0f},
                                   {0.0f, 1.0f, 0.0f}, /*speed=*/2.0f,
                                   /*fuse=*/0.5f, blast);
    REQUIRE(g.valid());

    std::vector<Entity> scratch;
    constexpr f32 dt = 1.0f / 60.0f;

    // Tick for less than the fuse: no detonation yet, victim untouched, fuse down.
    for (int i = 0; i < 20; ++i) {  // 20/60 s ~= 0.333 s < 0.5 s
        tick_grenades(w, dt, scratch);
        REQUIRE(w.get<Health>(victim)->hp == Catch::Approx(100.0f));  // safe so far
    }
    REQUIRE(w.get<Grenade>(g) != nullptr);                 // still ticking
    REQUIRE(w.get<Grenade>(g)->fuse_s < 0.5f);             // fuse has burned down
    REQUIRE(w.get<Grenade>(g)->fuse_s > 0.0f);             // but not yet expired
}

TEST_CASE("gameplay: on fuse expiry the grenade detonates a radial blast",
          "[gameplay]") {
    World w;
    const Entity thrower = w.create();
    // One victim at the detonation point (inside the inner radius -> full
    // damage), one well beyond the outer radius (untouched).
    const Entity core_victim = spawn_victim(w, {0.0f, 0.0f, 0.0f}, 100.0f);
    const Entity outside_victim =
        spawn_victim(w, {0.0f, 0.0f, 50.0f}, 100.0f);  // 50 m away

    const SplashParams blast{2.0f, 10.0f, 80.0f};
    // Zero speed: the grenade detonates essentially where it was thrown, on top
    // of `core_victim`. (Gravity over one short fuse leaves it inside the core.)
    const Entity g = throw_grenade(w, thrower, kNoTeam, {0.0f, 0.0f, 0.0f},
                                   {0.0f, 1.0f, 0.0f}, /*speed=*/0.0f,
                                   /*fuse=*/0.1f, blast);
    REQUIRE(g.valid());

    std::vector<Entity> scratch;
    constexpr f32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 12; ++i) tick_grenades(w, dt, scratch);  // > 0.1 s

    REQUIRE(w.get<Grenade>(g) == nullptr);                              // gone
    REQUIRE(w.get<Health>(core_victim)->hp == Catch::Approx(20.0f));    // full 80
    REQUIRE(w.get<Health>(outside_victim)->hp == Catch::Approx(100.0f));  // none
}

TEST_CASE("gameplay: the grenade entity is despawned after detonation",
          "[gameplay]") {
    World w;
    const Entity thrower = w.create();
    const SplashParams blast{2.0f, 10.0f, 80.0f};
    const Entity g = throw_grenade(w, thrower, kNoTeam, {0.0f, 0.0f, 0.0f},
                                   {1.0f, 0.0f, 0.0f}, /*speed=*/1.0f,
                                   /*fuse=*/0.05f, blast);
    REQUIRE(g.valid());
    REQUIRE(w.get<Grenade>(g) != nullptr);

    std::vector<Entity> scratch;
    tick_grenades(w, /*dt=*/0.1f, scratch);  // dt > fuse -> detonate this tick

    REQUIRE(w.get<Grenade>(g) == nullptr);          // Grenade component is gone
    REQUIRE_FALSE(w.alive(g));                       // entity fully destroyed
}

TEST_CASE("gameplay: the owner_team friendly filter spares teammates in the blast",
          "[gameplay]") {
    World w;
    const Entity thrower = w.create();
    // A teammate and an enemy both standing on the detonation point.
    const Entity mate = spawn_victim(w, {0.0f, 0.0f, 0.0f}, 100.0f);
    w.add(mate, Team{0u});
    const Entity enemy = spawn_victim(w, {0.0f, 0.0f, 0.0f}, 100.0f);
    w.add(enemy, Team{1u});

    const SplashParams blast{2.0f, 10.0f, 80.0f};
    // Thrower team 0: detonate among both, sparing the teammate, blasting the foe.
    const Entity g = throw_grenade(w, thrower, /*thrower_team=*/0,
                                   {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                                   /*speed=*/0.0f, /*fuse=*/0.05f, blast);
    REQUIRE(g.valid());

    std::vector<Entity> scratch;
    tick_grenades(w, /*dt=*/0.1f, scratch);

    REQUIRE(w.get<Health>(mate)->hp == Catch::Approx(100.0f));   // teammate spared
    REQUIRE(w.get<Health>(enemy)->hp == Catch::Approx(20.0f));   // enemy took 80
}

TEST_CASE("gameplay: throw_grenade with a zero direction returns invalid",
          "[gameplay]") {
    World w;
    const Entity thrower = w.create();
    const SplashParams blast{2.0f, 10.0f, 80.0f};
    const Entity g = throw_grenade(w, thrower, kNoTeam, {0.0f, 0.0f, 0.0f},
                                   {0.0f, 0.0f, 0.0f}, /*speed=*/10.0f,
                                   /*fuse=*/1.0f, blast);
    REQUIRE_FALSE(g.valid());  // no heading -> nothing thrown
}

TEST_CASE("gameplay: grenade throw + detonation is deterministic across worlds",
          "[gameplay][determinism]") {
    const auto run = []() {
        World w;
        const Entity thrower = w.create();
        // A spread of victims at varied 3D offsets, spawned in mixed order.
        std::vector<Entity> victims;
        for (int i = 0; i < 6; ++i) {
            const f32 f = static_cast<f32>(i);
            victims.push_back(
                spawn_victim(w, {f, f * 0.5f, -f * 0.25f}, 100.0f));
        }

        const SplashParams blast{1.5f, 8.0f, 70.0f};
        // Throw several grenades along different headings with different fuses.
        const Entity g0 = throw_grenade(w, thrower, kNoTeam, {0, 0, 0},
                                        {1, 1, 0}, 8.0f, 0.30f, blast);
        const Entity g1 = throw_grenade(w, thrower, kNoTeam, {2, 0, 0},
                                        {0, 1, 1}, 6.0f, 0.45f, blast);
        const Entity g2 = throw_grenade(w, thrower, kNoTeam, {1, 1, -1},
                                        {-1, 2, 0}, 5.0f, 0.20f, blast);

        std::vector<Entity> scratch;
        constexpr f32 dt = 1.0f / 120.0f;
        for (int step = 0; step < 90; ++step) tick_grenades(w, dt, scratch);

        // Capture every victim's surviving health plus each grenade's final fate
        // (alive grenades contribute their integrated position; detonated ones a
        // sentinel) — all of it must match bit-for-bit across the two runs.
        std::vector<f32> out;
        for (Entity e : victims) out.push_back(w.get<Health>(e)->hp);
        for (Entity gg : {g0, g1, g2}) {
            const Grenade* gc = w.get<Grenade>(gg);
            if (gc != nullptr) {
                const math::Vec3 p = pos_of(w, gg);
                out.push_back(p.x);
                out.push_back(p.y);
                out.push_back(p.z);
            } else {
                out.push_back(-1.0e9f);  // sentinel: detonated + despawned
            }
        }
        return out;
    };
    REQUIRE(run() == run());  // bit-identical across two independent worlds
}
