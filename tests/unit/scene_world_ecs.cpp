// SPDX-License-Identifier: MIT
// Lane 06 -- focused coverage for World's archetype/chunk ECS backing.

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace psynder;
using namespace psynder::scene;

namespace {

struct EcsPosition {
    f32 x;
    f32 y;
    f32 z;
};

struct EcsVelocity {
    f32 x;
    f32 y;
    f32 z;
};

struct alignas(32) EcsWide {
    u64 lane[4];
};

}  // namespace

TEST_CASE("scene World creates destroys and rejects stale entities", "[scene][world][ecs]") {
    World& world = World::Get();

    const Entity first = world.create();
    REQUIRE(first.valid());
    REQUIRE(world.alive(first));

    world.destroy(first);
    CHECK_FALSE(world.alive(first));

    const Entity second = world.create();
    REQUIRE(second.valid());
    REQUIRE(world.alive(second));
    CHECK_FALSE(world.alive(first));
    if (second.index() == first.index()) {
        CHECK(second.gen() != first.gen());
    }

    world.destroy(second);
    CHECK_FALSE(world.alive(second));
}

TEST_CASE("scene World migrates POD components across archetypes", "[scene][world][ecs]") {
    World& world = World::Get();

    std::vector<Entity> entities;
    entities.reserve(384);
    for (u32 i = 0; i < 384; ++i) {
        const Entity e = world.create();
        REQUIRE(e.valid());
        world.add(e, EcsPosition{static_cast<f32>(i), static_cast<f32>(i + 1u), static_cast<f32>(i + 2u)});
        if ((i & 1u) == 0u) {
            world.add(e, EcsVelocity{1.0f, 2.0f, 3.0f});
        }
        entities.push_back(e);
    }

    for (u32 i = 0; i < entities.size(); ++i) {
        EcsPosition* pos = world.get<EcsPosition>(entities[i]);
        REQUIRE(pos != nullptr);
        CHECK(pos->x == static_cast<f32>(i));
        CHECK(pos->y == static_cast<f32>(i + 1u));
        CHECK(pos->z == static_cast<f32>(i + 2u));

        EcsVelocity* vel = world.get<EcsVelocity>(entities[i]);
        if ((i & 1u) == 0u) {
            REQUIRE(vel != nullptr);
            CHECK(vel->z == 3.0f);
        } else {
            CHECK(vel == nullptr);
        }
    }

    world.add(entities[10], EcsPosition{42.0f, 43.0f, 44.0f});
    REQUIRE(world.get<EcsPosition>(entities[10]) != nullptr);
    CHECK(world.get<EcsPosition>(entities[10])->x == 42.0f);

    world.remove<EcsPosition>(entities[10]);
    CHECK(world.get<EcsPosition>(entities[10]) == nullptr);
    REQUIRE(world.get<EcsVelocity>(entities[10]) != nullptr);
    CHECK(world.get<EcsVelocity>(entities[10])->x == 1.0f);

    for (Entity e : entities) {
        world.destroy(e);
    }
}

TEST_CASE("scene World preserves swapped entities when removing components", "[scene][world][ecs]") {
    World& world = World::Get();
    const Entity a = world.create();
    const Entity b = world.create();
    const Entity c = world.create();

    world.add(a, EcsPosition{1.0f, 0.0f, 0.0f});
    world.add(b, EcsPosition{2.0f, 0.0f, 0.0f});
    world.add(c, EcsPosition{3.0f, 0.0f, 0.0f});
    world.add(a, EcsVelocity{10.0f, 0.0f, 0.0f});
    world.add(b, EcsVelocity{20.0f, 0.0f, 0.0f});
    world.add(c, EcsVelocity{30.0f, 0.0f, 0.0f});

    world.remove<EcsVelocity>(b);

    REQUIRE(world.get<EcsPosition>(a) != nullptr);
    REQUIRE(world.get<EcsPosition>(b) != nullptr);
    REQUIRE(world.get<EcsPosition>(c) != nullptr);
    REQUIRE(world.get<EcsVelocity>(a) != nullptr);
    CHECK(world.get<EcsVelocity>(b) == nullptr);
    REQUIRE(world.get<EcsVelocity>(c) != nullptr);
    CHECK(world.get<EcsPosition>(c)->x == 3.0f);
    CHECK(world.get<EcsVelocity>(c)->x == 30.0f);

    world.destroy(a);
    world.destroy(b);
    world.destroy(c);
}

TEST_CASE("scene World stores aligned component columns", "[scene][world][ecs]") {
    World& world = World::Get();
    const Entity e = world.create();
    REQUIRE(e.valid());

    world.add(e, EcsWide{{11u, 22u, 33u, 44u}});
    EcsWide* wide = world.get<EcsWide>(e);
    REQUIRE(wide != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(wide) % alignof(EcsWide) == 0u);
    CHECK(wide->lane[2] == 33u);

    world.destroy(e);
}
