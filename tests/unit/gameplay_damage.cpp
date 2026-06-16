// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_damage.cpp — deterministic FPS damage / death / respawn.
// Covers: armor absorbs the Quake ratio then health takes the rest; lethal
// damage tags Dead; respawn timers restore health + move to spawn; and the whole
// thing is bit-reproducible.

#include "gameplay/Damage.h"
#include "gameplay/GameplayComponents.h"

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
Entity spawn_actor(World& w, f32 hp, f32 armor, math::Vec3 spawn, f32 delay) {
    const Entity e = w.create();
    w.add(e, Health{hp, hp});
    if (armor > 0.0f) w.add(e, Armor{armor});
    w.add(e, Respawnable{spawn, delay});
    TransformWS t{};
    t.mtw = math::translate({0.0f, 0.0f, 0.0f});
    t.prev_mtw = t.mtw;
    w.add(e, t);
    return e;
}
}  // namespace

TEST_CASE("gameplay: damage hits health directly without armor", "[gameplay]") {
    World w;
    const Entity e = spawn_actor(w, 100.0f, 0.0f, {0, 0, 0}, 3.0f);
    REQUIRE_FALSE(apply_damage(w, e, 30.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(70.0f));
    REQUIRE(w.get<Dead>(e) == nullptr);
}

TEST_CASE("gameplay: armor absorbs the Quake ratio, capped by its points",
          "[gameplay]") {
    World w;
    // 60 dmg: armor wants 2/3*60 = 40 absorbed, has 50 -> absorbs 40, 20 to hp.
    const Entity e = spawn_actor(w, 100.0f, 50.0f, {0, 0, 0}, 3.0f);
    REQUIRE_FALSE(apply_damage(w, e, 60.0f));
    REQUIRE(w.get<Armor>(e)->points == Catch::Approx(10.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(80.0f));

    // Now armor (10) is less than 2/3*60=40 -> absorbs only 10, 50 to hp.
    REQUIRE_FALSE(apply_damage(w, e, 60.0f));
    REQUIRE(w.get<Armor>(e)->points == Catch::Approx(0.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(30.0f));
}

TEST_CASE("gameplay: lethal damage tags Dead with the respawn delay",
          "[gameplay]") {
    World w;
    const Entity e = spawn_actor(w, 25.0f, 0.0f, {7, 0, -2}, 2.0f);
    REQUIRE(apply_damage(w, e, 40.0f));  // kills
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(0.0f));
    const Dead* d = w.get<Dead>(e);
    REQUIRE(d != nullptr);
    REQUIRE(d->respawn_in_s == Catch::Approx(2.0f));
    // Further damage to a corpse does nothing.
    REQUIRE_FALSE(apply_damage(w, e, 10.0f));
}

TEST_CASE("gameplay: respawn restores health + moves to spawn after the delay",
          "[gameplay]") {
    World w;
    const math::Vec3 spawn{7.0f, 1.0f, -2.0f};
    const Entity e = spawn_actor(w, 100.0f, 0.0f, spawn, 2.0f);
    REQUIRE(apply_damage(w, e, 150.0f));  // dead, 2 s timer

    std::vector<Entity> scratch;
    constexpr f32 dt = 1.0f / 120.0f;
    // Step ~1.9 s: still dead.
    for (int i = 0; i < 228; ++i) update_respawns(w, dt, scratch);
    REQUIRE(w.get<Dead>(e) != nullptr);
    // Cross 2 s: respawns.
    for (int i = 0; i < 20; ++i) update_respawns(w, dt, scratch);
    REQUIRE(w.get<Dead>(e) == nullptr);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(100.0f));
    const TransformWS* t = w.get<TransformWS>(e);
    REQUIRE(t->mtw.m[12] == Catch::Approx(spawn.x));
    REQUIRE(t->mtw.m[13] == Catch::Approx(spawn.y));
    REQUIRE(t->mtw.m[14] == Catch::Approx(spawn.z));
}

TEST_CASE("gameplay: damage + respawn is deterministic across worlds",
          "[gameplay][determinism]") {
    const auto run = []() {
        World w;
        std::vector<Entity> es;
        for (int i = 0; i < 8; ++i) {
            es.push_back(spawn_actor(w, 100.0f, 50.0f,
                                     {static_cast<f32>(i), 0.0f, 0.0f}, 1.0f));
        }
        std::vector<Entity> scratch;
        constexpr f32 dt = 1.0f / 120.0f;
        for (int step = 0; step < 200; ++step) {
            // Deterministic damage pattern (no RNG).
            for (std::size_t i = 0; i < es.size(); ++i) {
                apply_damage(w, es[i], 1.0f + static_cast<f32>((step + i) % 5));
            }
            update_respawns(w, dt, scratch);
        }
        std::vector<f32> out;
        for (Entity e : es) {
            out.push_back(w.get<Health>(e)->hp);
            out.push_back(w.get<Armor>(e) ? w.get<Armor>(e)->points : -1.0f);
        }
        return out;
    };
    const std::vector<f32> a = run();
    const std::vector<f32> b = run();
    REQUIRE(a == b);
}
