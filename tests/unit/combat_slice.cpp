// SPDX-License-Identifier: MIT
// Exercises the reference FPS combat slice end to end on the ECS spine:
// query -> hitscan raycast -> deferred command buffer. Headless + deterministic.

// Reference gameplay module lives under samples/ (renderer-agnostic, reusable).
#include "../../samples/combat/Combat.h"

#include <catch2/catch_test_macros.hpp>

using psynder::Entity;
using psynder::math::Vec3;
using psynder::scene::CommandBuffer;
using psynder::scene::World;
namespace combat = psynder::combat;

namespace {

Entity spawn_target(World& w, Vec3 pos, float hp) {
    const Entity e = w.create();
    w.add(e, combat::WorldPos{pos});
    w.add(e, combat::BoxCollider{{0.5f, 0.5f, 0.5f}});
    w.add(e, combat::Health{hp, hp});
    return e;
}

std::size_t count_pickups(World& w, Vec3& out_first_pos) {
    std::size_t n = 0;
    w.for_each_chunk<combat::Pickup, combat::WorldPos>(
        [&](std::size_t count, combat::Pickup*, combat::WorldPos* pos) {
            if (n == 0 && count > 0) out_first_pos = pos[0].p;
            n += count;
        });
    return n;
}

}  // namespace

TEST_CASE("combat: hitscan hits the nearest target along the ray",
          "[combat][ecs]") {
    World w;
    const Entity near_target = spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f);
    /* far target */          spawn_target(w, {10.0f, 0.0f, 0.0f}, 100.0f);

    const combat::Ray shot{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    const combat::Hit hit = combat::raycast_nearest(w, shot, 100.0f);

    REQUIRE(hit.hit);
    REQUIRE(hit.entity == near_target);   // nearest, not the far one
    REQUIRE(hit.t > 4.0f);
    REQUIRE(hit.t < 5.0f);                // entry face of the near box at x=4.5
}

TEST_CASE("combat: a ray that misses everything reports no hit", "[combat][ecs]") {
    World w;
    spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f);

    const combat::Ray up{{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};  // nothing above
    REQUIRE_FALSE(combat::raycast_nearest(w, up, 100.0f).hit);
}

TEST_CASE("combat: sustained fire kills a target; reaper destroys it and drops a pickup",
          "[combat][ecs][commandbuffer]") {
    World w;
    const Entity victim = spawn_target(w, {5.0f, 0.0f, 0.0f}, 100.0f);
    const Entity bystander = spawn_target(w, {10.0f, 0.0f, 0.0f}, 100.0f);

    const combat::Ray shot{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    CommandBuffer cb;

    // Four shots of 30 damage -> -20 hp. Every shot hits the nearer victim;
    // it stays collidable until the reaper runs.
    for (int s = 0; s < 4; ++s) {
        const combat::Hit hit = combat::raycast_nearest(w, shot, 100.0f);
        REQUIRE(hit.entity == victim);
        combat::apply_damage(w, hit, 30.0f);
    }
    REQUIRE(w.get<combat::Health>(victim)->hp < 0.0f);

    // No structural change has happened yet — the reaper only records.
    combat::reap_dead(w, cb);
    REQUIRE(w.alive(victim));
    cb.playback(w);

    REQUIRE_FALSE(w.alive(victim));               // destroyed at the sync point
    REQUIRE(w.alive(bystander));                  // never hit
    REQUIRE(w.get<combat::Health>(bystander)->hp == 100.0f);

    Vec3 pickup_pos{};
    REQUIRE(count_pickups(w, pickup_pos) == 1);   // one drop, spawned via temp handle
    REQUIRE(pickup_pos.x == 5.0f);                // where the victim died

    // With the victim gone, the next shot now strikes the (formerly far) bystander.
    REQUIRE(combat::raycast_nearest(w, shot, 100.0f).entity == bystander);
}

TEST_CASE("combat: the full tick is deterministic across identical worlds",
          "[combat][ecs][determinism]") {
    auto run = [](World& w) {
        spawn_target(w, {5.0f, 0.0f, 0.0f}, 60.0f);
        spawn_target(w, {-5.0f, 0.0f, 0.0f}, 60.0f);
        CommandBuffer cb;
        const combat::Ray fwd{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
        const combat::Ray back{{0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}};
        for (int s = 0; s < 3; ++s) {  // 90 dmg each side -> both die
            combat::apply_damage(w, combat::raycast_nearest(w, fwd, 100.0f), 30.0f);
            combat::apply_damage(w, combat::raycast_nearest(w, back, 100.0f), 30.0f);
        }
        combat::reap_dead(w, cb);
        cb.playback(w);
    };

    World a;
    World b;
    run(a);
    run(b);

    Vec3 pa{};
    Vec3 pb{};
    REQUIRE(count_pickups(a, pa) == 2);
    REQUIRE(count_pickups(b, pb) == 2);
    REQUIRE(pa.x == pb.x);  // identical inputs -> identical world state
    REQUIRE(pa.y == pb.y);
    REQUIRE(pa.z == pb.z);
}
