// SPDX-License-Identifier: MIT
// Locks in two ECS determinism/robustness guarantees:
//   * a recycled entity slot yields a fresh generation, so a stale handle can
//     never alias the new live entity (32-bit generation -> no practical ABA);
//   * World state is per-instance, not a process-global, so worlds are isolated
//     (a server can create/tear down match worlds; replay can spin a clean one).

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>

using psynder::Entity;
using psynder::scene::World;

TEST_CASE("scene/world: recycled slot bumps generation, stale handle stays dead",
          "[scene][ecs][determinism]") {
    World w;

    const Entity a = w.create();
    REQUIRE(a.valid());
    REQUIRE(w.alive(a));

    w.destroy(a);
    REQUIRE_FALSE(w.alive(a));

    // The free list is LIFO, so the next create reuses a's slot/index.
    const Entity b = w.create();
    REQUIRE(w.alive(b));
    REQUIRE(b.index() == a.index());   // same slot recycled
    REQUIRE(b.gen() != a.gen());       // but a fresh generation
    REQUIRE_FALSE(w.alive(a));         // the stale handle does NOT alias b
    REQUIRE_FALSE(a == b);
}

TEST_CASE("scene/world: worlds have independent entity tables",
          "[scene][ecs][isolation]") {
    // Entity handles are world-relative (not globally unique), so isolation is
    // proven via independent slot allocation, not by cross-world handle checks:
    // a process-global table would give w2's first entity index 2, not 0.
    World w1;
    World w2;

    const Entity a0 = w1.create();
    const Entity a1 = w1.create();
    REQUIRE(a0.index() == 0u);
    REQUIRE(a1.index() == 1u);

    const Entity b0 = w2.create();
    REQUIRE(b0.index() == 0u);  // w2 untouched by w1's two allocations
    REQUIRE(w2.alive(b0));

    // Mutating one world leaves the other intact.
    w1.destroy(a0);
    REQUIRE_FALSE(w1.alive(a0));
    REQUIRE(w1.alive(a1));
    REQUIRE(w2.alive(b0));
}
