// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/destruction_fracture_spawn.cpp
//
// End-to-end for the destruction feature (ADR-021/#45): a shot static crate is
// turned into a burst of Jolt dynamic shards (spawn_fracture_shards). Headless
// equivalent of "shoot a crate, it breaks": we spawn the shards, step the Jolt
// world, sync back to the ECS, and assert the pieces actually exist, take real
// physics (burst apart + fall), and that the spawn is deterministic in its seed.

#include "physics/destruction/FractureSpawn.h"

#include "physics/core/CharacterSpine.h"
#include "physics/core/EcsCharacterBridge.h"

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

namespace m = psynder::math;
namespace spine = psynder::physics::character_spine;
namespace dz = psynder::physics::destruction;
using psynder::Entity;
using psynder::scene::Collider;
using psynder::scene::PropDesc;
using psynder::scene::RenderMaterial;
using psynder::scene::ShapeKind;
using psynder::scene::TransformWS;
using psynder::scene::World;
using psynder::scene::spawn_prop;

namespace {

Entity add_ground(World& w) {
    PropDesc g;
    g.position = {0.0f, -0.5f, 0.0f};
    g.shape = ShapeKind::Box;
    g.half_extents = {50.0f, 0.5f, 50.0f};  // top face at y == 0
    return spawn_prop(w, g);
}

// World position of a dynamic body's ECS transform (translation column).
m::Vec3 ecs_pos(World& w, Entity e) {
    const TransformWS* xf = w.get<TransformWS>(e);
    REQUIRE(xf != nullptr);
    return {xf->mtw.m[12], xf->mtw.m[13], xf->mtw.m[14]};
}

}  // namespace

TEST_CASE("fracture spawn: a shot crate bursts into dynamic shards that move",
          "[destruction][fracture][spawn][ecs]") {
    World w;
    add_ground(w);

    // A 1 m static crate resting on the ground (centre at y = 0.5).
    PropDesc crate;
    crate.position = {0.0f, 0.5f, 0.0f};
    crate.shape = ShapeKind::Box;
    crate.half_extents = {0.5f, 0.5f, 0.5f};
    const Entity crate_e = spawn_prop(w, crate);

    spine::WorldDesc desc{};
    desc.max_bodies = 128;
    spine::World* pw = spine::create_world(desc);
    REQUIRE(pw != nullptr);

    const auto statics = psynder::physics::build_jolt_statics_from_ecs(pw, w);
    REQUIRE(statics.size() == 2);  // ground + crate

    // Snapshot the crate before we "destroy" it, then fracture it.
    const TransformWS crate_xf = *w.get<TransformWS>(crate_e);
    const Collider crate_col = *w.get<Collider>(crate_e);
    const RenderMaterial look{{0.8f, 0.5f, 0.2f}, 0.7f, 0.0f, {0.0f, 0.0f, 0.0f}, 0.0f};

    std::vector<psynder::physics::EcsDynamicBody> shards;
    const m::Vec3 shot_dir{0.0f, 0.0f, 1.0f};  // straight along +Z
    const psynder::u32 spawned = dz::spawn_fracture_shards(
        pw, w, crate_xf, crate_col, look, shot_dir, /*seed=*/0xC0FFEEull, shards);

    REQUIRE(spawned == 8u);             // 2x2x2 default grid
    REQUIRE(shards.size() == 8u);

    // Mimic the in-game path: drop the crate's static body + ECS entity.
    spine::StaticBodyHandle crate_handle{};
    for (const auto& [e, h] : statics) if (e == crate_e) crate_handle = h;
    REQUIRE(crate_handle.valid());
    REQUIRE(spine::remove_static_body(pw, crate_handle));
    w.destroy(crate_e);

    // Every shard is a live ECS dynamic body, initially clustered in the crate.
    for (const auto& [e, h] : shards) {
        REQUIRE(h.valid());
        REQUIRE(w.get<psynder::scene::DynamicBody>(e) != nullptr);
    }
    const m::Vec3 source_centre{0.0f, 0.5f, 0.0f};
    for (const auto& [e, h] : shards) {
        const m::Vec3 p = ecs_pos(w, e);
        // Each base shard centroid sits within +-0.25 of the centre (+jitter).
        REQUIRE(m::length(m::sub(p, source_centre)) < 0.6f);
    }

    // Step ~2 s, syncing the solved transforms back into the ECS each tick.
    for (int i = 0; i < 240; ++i) {
        spine::step_fixed(pw);
        psynder::physics::sync_dynamics_to_ecs(pw, w, shards);
    }

    // The shards took real physics: they burst apart (at least one travelled well
    // beyond the original crate footprint) and none tunnelled through the ground.
    float max_horiz = 0.0f;
    for (const auto& [e, h] : shards) {
        const m::Vec3 p = ecs_pos(w, e);
        REQUIRE(std::isfinite(p.x));
        REQUIRE(std::isfinite(p.y));
        REQUIRE(std::isfinite(p.z));
        REQUIRE(p.y > -0.5f);  // didn't fall through the floor
        const float horiz =
            std::sqrt((p.x - source_centre.x) * (p.x - source_centre.x) +
                      (p.z - source_centre.z) * (p.z - source_centre.z));
        max_horiz = std::max(max_horiz, horiz);
    }
    REQUIRE(max_horiz > 0.8f);  // they actually scattered, not a frozen cluster

    spine::destroy_world(pw);
}

TEST_CASE("fracture spawn: identical seed yields identical shard placement",
          "[destruction][fracture][spawn][determinism]") {
    auto run = [](std::vector<m::Vec3>& out) {
        World w;
        add_ground(w);
        spine::WorldDesc desc{};
        desc.max_bodies = 128;
        spine::World* pw = spine::create_world(desc);
        REQUIRE(pw != nullptr);

        TransformWS xf{};
        xf.mtw = psynder::scene::mat4_trs({3.0f, 2.0f, -1.0f},
                                          {0.0f, 0.0f, 0.0f, 1.0f},
                                          {1.0f, 1.0f, 1.0f});
        const Collider col{ShapeKind::Box, {0.5f, 0.5f, 0.5f}};
        const RenderMaterial look{{0.8f, 0.8f, 0.8f}, 0.5f, 0.0f, {0, 0, 0}, 0.0f};

        std::vector<psynder::physics::EcsDynamicBody> shards;
        dz::spawn_fracture_shards(pw, w, xf, col, look, {0.0f, 0.0f, 1.0f},
                                  /*seed=*/0xABCDEF01ull, shards);
        out.clear();
        for (const auto& [e, h] : shards) out.push_back(ecs_pos(w, e));
        spine::destroy_world(pw);
    };

    std::vector<m::Vec3> a;
    std::vector<m::Vec3> b;
    run(a);
    run(b);

    REQUIRE(a.size() == 8u);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].x == b[i].x);
        REQUIRE(a[i].y == b[i].y);
        REQUIRE(a[i].z == b[i].z);
    }
}
