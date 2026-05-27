// SPDX-License-Identifier: MIT
// Exercises the deferred command buffer (ADR-018 effect bucket): destroy during
// iteration, spawn-with-components via temp handles, deferred add/remove,
// create-then-destroy in one batch, and pooled (no steady-state alloc) replay.

#include "scene/CommandBuffer.h"
#include "scene/GxComponents.h"
#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using psynder::Entity;
using psynder::scene::CommandBuffer;
using psynder::scene::LightPoint;
using psynder::scene::VisibleBit;
using psynder::scene::World;

namespace {

// An over-aligned component: exercises the command-buffer payload path against
// alignment larger than the byte arena's default allocation alignment.
PSYNDER_COMPONENT(OverAligned) {
    alignas(64) float data[4];
    int tag;
};


LightPoint make_light(float intensity) {
    LightPoint lp{};
    lp.position = {0.0f, 0.0f, 0.0f};
    lp.radius = 1.0f;
    lp.color = {1.0f, 1.0f, 1.0f};
    lp.intensity = intensity;
    return lp;
}

std::size_t count_lights(World& w) {
    std::size_t n = 0;
    w.for_each_chunk<LightPoint>([&](std::size_t count, LightPoint*) { n += count; });
    return n;
}

}  // namespace

TEST_CASE("scene/command-buffer: destroy recorded during iteration applies at playback",
          "[scene][ecs][commandbuffer]") {
    World w;
    for (int i = 0; i < 6; ++i) {
        w.add(w.create(), make_light(static_cast<float>(i) * 10.0f));  // 0,10,20,30,40,50
    }
    REQUIRE(count_lights(w) == 6);

    CommandBuffer cb;
    // Recording during iteration is safe — nothing mutates the World yet.
    w.for_each_chunk_with_entities<LightPoint>(
        [&](std::size_t n, const Entity* ents, LightPoint* lp) {
            for (std::size_t i = 0; i < n; ++i) {
                if (lp[i].intensity < 25.0f) cb.destroy(ents[i]);  // 0,10,20 -> three
            }
        });
    REQUIRE(count_lights(w) == 6);  // not applied yet

    cb.playback(w);
    REQUIRE(count_lights(w) == 3);  // the three dim lights are gone
    REQUIRE(cb.empty());            // playback cleared the buffer
}

TEST_CASE("scene/command-buffer: spawn via temp handle populates components at playback",
          "[scene][ecs][commandbuffer]") {
    World w;
    CommandBuffer cb;

    const Entity temp = cb.create();
    cb.add(temp, make_light(777.0f));
    cb.add(temp, VisibleBit{});
    cb.playback(w);

    std::size_t both = 0;
    w.for_each_chunk<LightPoint, VisibleBit>([&](std::size_t n, LightPoint* lp, VisibleBit*) {
        both += n;
        for (std::size_t i = 0; i < n; ++i) REQUIRE(lp[i].intensity == 777.0f);
    });
    REQUIRE(both == 1);
}

TEST_CASE("scene/command-buffer: deferred add and remove on an existing entity",
          "[scene][ecs][commandbuffer]") {
    World w;
    const Entity e = w.create();
    w.add(e, make_light(5.0f));

    CommandBuffer cb;
    cb.add(e, VisibleBit{});
    cb.playback(w);
    REQUIRE(w.get<VisibleBit>(e) != nullptr);
    REQUIRE(w.get<LightPoint>(e) != nullptr);

    cb.remove<LightPoint>(e);
    cb.playback(w);
    REQUIRE(w.get<LightPoint>(e) == nullptr);
    REQUIRE(w.alive(e));  // still alive, just shed a component
}

TEST_CASE("scene/command-buffer: create-then-destroy a temp in one batch nets nothing",
          "[scene][ecs][commandbuffer]") {
    World w;
    CommandBuffer cb;

    const Entity temp = cb.create();
    cb.add(temp, make_light(1.0f));
    cb.destroy(temp);  // destroy resolves to the entity create() makes at playback
    cb.playback(w);

    REQUIRE(count_lights(w) == 0);
}

TEST_CASE("scene/command-buffer: steady-state replay does not reallocate",
          "[scene][ecs][commandbuffer][nogc]") {
    World w;
    CommandBuffer cb;
    cb.reserve(64, 8192);

    auto record_frame = [&] {
        for (int i = 0; i < 16; ++i) {
            const Entity t = cb.create();
            cb.add(t, make_light(static_cast<float>(i)));
        }
    };

    record_frame();
    cb.playback(w);  // warm-up establishes the pooled high-water mark
    const std::size_t cc = cb.command_capacity();
    const std::size_t pc = cb.payload_capacity();

    for (int frame = 0; frame < 256; ++frame) {
        record_frame();
        cb.playback(w);
        REQUIRE(cb.command_capacity() == cc);  // pooled — no realloc
        REQUIRE(cb.payload_capacity() == pc);
    }
}

TEST_CASE("scene/command-buffer: over-aligned component payload survives playback",
          "[scene][ecs][commandbuffer][align]") {
    World w;
    CommandBuffer cb;

    const Entity t = cb.create();
    OverAligned v{};
    v.data[0] = 1.5f;
    v.tag = 42;
    cb.add(t, v);
    cb.playback(w);

    std::size_t seen = 0;
    w.for_each_chunk<OverAligned>([&](std::size_t n, OverAligned* o) {
        seen += n;
        REQUIRE(reinterpret_cast<std::uintptr_t>(o) % alignof(OverAligned) == 0u);
        REQUIRE(o[0].data[0] == 1.5f);
        REQUIRE(o[0].tag == 42);
    });
    REQUIRE(seen == 1);
}
