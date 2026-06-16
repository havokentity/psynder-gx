// SPDX-License-Identifier: MIT
// Exercises the archetype chunk-query / iteration API on World — the system
// hot path. Verifies cross-archetype iteration, in-place column mutation,
// multi-component filtering, entity-id access, and empty results.

#include "scene/GxComponents.h"
#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using psynder::Entity;
using psynder::scene::LightPoint;
using psynder::scene::VisibleBit;
using psynder::scene::World;

namespace {

LightPoint make_light(float x, float intensity) {
    LightPoint lp{};
    lp.position = {x, 0.0f, 0.0f};
    lp.radius = 1.0f;
    lp.color = {1.0f, 1.0f, 1.0f};
    lp.intensity = intensity;
    return lp;
}

}  // namespace

TEST_CASE("scene/world: for_each_chunk iterates all matching archetypes and mutates in place",
          "[scene][ecs][query]") {
    World w;

    std::vector<Entity> lights;
    for (int i = 0; i < 5; ++i) {
        const Entity e = w.create();
        w.add(e, make_light(static_cast<float>(i), 100.0f));
        lights.push_back(e);
    }
    // Give three of them a VisibleBit — they migrate to a second archetype
    // {LightPoint, VisibleBit}. A LightPoint query must still visit all five.
    for (int i = 0; i < 3; ++i) {
        w.add(lights[static_cast<std::size_t>(i)], VisibleBit{});
    }
    // An entity with no LightPoint must never be visited by a LightPoint query.
    const Entity bare = w.create();
    w.add(bare, VisibleBit{});

    std::size_t seen = 0;
    w.for_each_chunk<LightPoint>([&](std::size_t n, LightPoint* lp) {
        seen += n;
        for (std::size_t i = 0; i < n; ++i) {
            lp[i].intensity += 1.0f;  // in-place mutation of live chunk memory
        }
    });
    REQUIRE(seen == 5);  // visited both archetypes containing LightPoint

    for (const Entity e : lights) {
        const LightPoint* lp = w.get<LightPoint>(e);
        REQUIRE(lp != nullptr);
        REQUIRE(lp->intensity == 101.0f);  // mutation persisted in storage
    }
    REQUIRE(w.get<LightPoint>(bare) == nullptr);
}

TEST_CASE("scene/world: multi-component query filters to the intersection",
          "[scene][ecs][query]") {
    World w;
    for (int i = 0; i < 5; ++i) {
        const Entity e = w.create();
        w.add(e, make_light(0.0f, 50.0f));
        if (i < 2) w.add(e, VisibleBit{});  // only 2 carry both
    }

    std::size_t both = 0;
    w.for_each_chunk<LightPoint, VisibleBit>([&](std::size_t n, LightPoint*, VisibleBit*) {
        both += n;
    });
    REQUIRE(both == 2);
}

TEST_CASE("scene/world: for_each_chunk_with_entities yields the chunk's entity ids",
          "[scene][ecs][query]") {
    World w;
    std::vector<Entity> created;
    for (int i = 0; i < 4; ++i) {
        const Entity e = w.create();
        w.add(e, make_light(static_cast<float>(i), 10.0f));
        created.push_back(e);
    }

    std::vector<Entity> visited;
    w.for_each_chunk_with_entities<LightPoint>(
        [&](std::size_t n, const Entity* ents, LightPoint* lp) {
            for (std::size_t i = 0; i < n; ++i) {
                visited.push_back(ents[i]);
                // The entity at row i owns the LightPoint at column row i.
                REQUIRE(w.get<LightPoint>(ents[i])->position.x == lp[i].position.x);
            }
        });

    std::sort(visited.begin(), visited.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });
    std::sort(created.begin(), created.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });
    REQUIRE(visited == created);
}

TEST_CASE("scene/world: query for an absent component visits nothing",
          "[scene][ecs][query]") {
    World w;
    for (int i = 0; i < 3; ++i) {
        w.add(w.create(), VisibleBit{});  // no LightPoint anywhere
    }
    std::size_t seen = 0;
    w.for_each_chunk<LightPoint>([&](std::size_t n, LightPoint*) { seen += n; });
    REQUIRE(seen == 0);
}
