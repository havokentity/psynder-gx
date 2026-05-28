// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/physics_character_controller.cpp
//
// The player is an ECS entity (spawn_character) whose capsule is simulated by
// Jolt (CharacterVirtual via character_spine) with the static world projected
// from the ECS Collider entities (EcsCharacterBridge). These cover: the ECS
// representation, the transform->rotation extraction, and that Jolt collision
// built from the ECS actually settles the capsule on the ground and blocks it
// against a wall. (Replaces the old hand-rolled AABB resolver — see ADR-019.)

#include "physics/core/CharacterController.h"
#include "physics/core/EcsCharacterBridge.h"
#include "physics/core/CharacterSpine.h"

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

namespace m = psynder::math;
namespace spine = psynder::physics::character_spine;
using psynder::Entity;
using psynder::physics::CharacterController;
using psynder::physics::CharacterDesc;
using psynder::physics::CharacterInput;
using psynder::physics::spawn_character;
using psynder::scene::Collider;
using psynder::scene::PropDesc;
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

// Build a Jolt world from the ECS + a capsule at `foot`. Caller destroys.
spine::World* make_phys(World& ecs, m::Vec3 foot, spine::Character** out_pawn) {
    spine::WorldDesc desc{};
    desc.max_bodies = 64;
    spine::World* pw = spine::create_world(desc);
    REQUIRE(pw != nullptr);
    psynder::physics::build_jolt_statics_from_ecs(pw, ecs);
    spine::CapsuleDesc cap{};
    cap.foot_position_m[0] = foot.x;
    cap.foot_position_m[1] = foot.y;
    cap.foot_position_m[2] = foot.z;
    cap.radius_m = 0.38f;
    cap.height_m = 1.82f;
    cap.max_horizontal_speed_mps = 6.0f;
    *out_pawn = spine::create_capsule_character(pw, cap);
    REQUIRE(*out_pawn != nullptr);
    return pw;
}

}  // namespace

TEST_CASE("character: spawn_character yields the canonical player components",
          "[physics][ecs][character]") {
    World w;
    CharacterDesc d;
    d.foot_position = {1.0f, 2.0f, -3.0f};
    const Entity e = spawn_character(w, d);
    REQUIRE(w.get<TransformWS>(e) != nullptr);
    REQUIRE(w.get<CharacterController>(e) != nullptr);
    REQUIRE(w.get<CharacterInput>(e) != nullptr);
    REQUIRE(w.get<TransformWS>(e)->mtw.m[12] == 1.0f);
    REQUIRE(w.get<TransformWS>(e)->mtw.m[13] == 2.0f);
    REQUIRE(w.get<TransformWS>(e)->mtw.m[14] == -3.0f);
    REQUIRE(w.get<CharacterController>(e)->radius_m == Catch::Approx(0.38f));
}

TEST_CASE("character: rotation_from_transform recovers the prop rotation",
          "[physics][ecs][character]") {
    // 45 deg about Y -> quat (0, sin22.5, 0, cos22.5).
    TransformWS xf{};
    xf.mtw = psynder::scene::mat4_trs(
        {0.0f, 0.0f, 0.0f},
        m::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 45.0f * m::kDegToRad),
        {1.0f, 1.0f, 1.0f});
    const m::Quat q = psynder::physics::rotation_from_transform(xf);
    REQUIRE(q.w == Catch::Approx(std::cos(22.5f * m::kDegToRad)).margin(1e-3));
    REQUIRE(q.y == Catch::Approx(std::sin(22.5f * m::kDegToRad)).margin(1e-3));
    REQUIRE(q.x == Catch::Approx(0.0f).margin(1e-3));
    REQUIRE(q.z == Catch::Approx(0.0f).margin(1e-3));
}

TEST_CASE("character: Jolt statics from ECS settle the capsule on the ground",
          "[physics][ecs][character]") {
    World w;
    add_ground(w);
    spine::Character* pawn = nullptr;
    spine::World* pw = make_phys(w, {0.0f, 2.0f, 0.0f}, &pawn);

    for (int i = 0; i < 360; ++i) spine::step_fixed(pw);  // ~3 s to settle

    const spine::Transform t = spine::character_transform(pawn);
    REQUIRE(t.foot_position_m[1] > -0.1f);
    REQUIRE(t.foot_position_m[1] < 0.2f);
    spine::destroy_world(pw);
}

TEST_CASE("character: a wall projected from the ECS blocks the capsule",
          "[physics][ecs][character]") {
    World w;
    add_ground(w);
    PropDesc wall;
    wall.position = {2.0f, 0.5f, 0.0f};
    wall.shape = ShapeKind::Box;
    wall.half_extents = {0.5f, 1.0f, 5.0f};  // near face at x == 1.5
    spawn_prop(w, wall);

    spine::Character* pawn = nullptr;
    spine::World* pw = make_phys(w, {0.0f, 0.5f, 0.0f}, &pawn);

    for (int i = 0; i < 360; ++i) {
        spine::set_desired_horizontal_velocity(pawn, 4.0f, 0.0f);  // walk +X
        spine::step_fixed(pw);
    }

    const spine::Transform t = spine::character_transform(pawn);
    REQUIRE(t.foot_position_m[0] < 1.4f);  // stopped at the wall, didn't tunnel
    spine::destroy_world(pw);
}

TEST_CASE("character: removing a static body drops its collider (no ghost)",
          "[physics][ecs][character]") {
    // Same wall as above, but we destroy its Jolt body via the handle returned
    // from build_jolt_statics_from_ecs — the capsule must then walk through the
    // space the wall occupied, proving no invisible collider lingers behind a
    // destroyed prop (the collision-ghost fix).
    World w;
    add_ground(w);
    PropDesc wall;
    wall.position = {2.0f, 0.5f, 0.0f};
    wall.shape = ShapeKind::Box;
    wall.half_extents = {0.5f, 1.0f, 5.0f};  // spans x in [1.5, 2.5]
    const Entity wall_e = spawn_prop(w, wall);

    spine::WorldDesc desc{};
    desc.max_bodies = 64;
    spine::World* pw = spine::create_world(desc);
    REQUIRE(pw != nullptr);

    const auto mapping = psynder::physics::build_jolt_statics_from_ecs(pw, w);
    REQUIRE(mapping.size() == 2);  // ground + wall

    spine::StaticBodyHandle wall_handle{};
    for (const auto& [entity, handle] : mapping) {
        if (entity == wall_e) wall_handle = handle;
    }
    REQUIRE(wall_handle.valid());

    REQUIRE(spine::remove_static_body(pw, wall_handle));
    REQUIRE_FALSE(spine::remove_static_body(pw, wall_handle));  // already gone
    REQUIRE_FALSE(spine::remove_static_body(pw, spine::StaticBodyHandle{}));

    spine::CapsuleDesc cap{};
    cap.foot_position_m[0] = 0.0f;
    cap.foot_position_m[1] = 0.5f;
    cap.foot_position_m[2] = 0.0f;
    cap.radius_m = 0.38f;
    cap.height_m = 1.82f;
    cap.max_horizontal_speed_mps = 6.0f;
    spine::Character* pawn = spine::create_capsule_character(pw, cap);
    REQUIRE(pawn != nullptr);

    for (int i = 0; i < 360; ++i) {
        spine::set_desired_horizontal_velocity(pawn, 4.0f, 0.0f);  // walk +X
        spine::step_fixed(pw);
    }

    const spine::Transform t = spine::character_transform(pawn);
    REQUIRE(t.foot_position_m[0] > 2.5f);  // walked clean through the old wall
    spine::destroy_world(pw);
}
