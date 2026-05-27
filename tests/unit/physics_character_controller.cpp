// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/physics_character_controller.cpp
//
// Scene-on-ECS migration, step 4: the player capsule + collision against the
// static scene run as ECS systems over scene::World (no parallel
// character_spine world). Covers: gravity settles the capsule onto the
// ground, horizontal input moves it, a static box blocks it, jumping launches
// it, and — the determinism pillar — two identical worlds tick to
// bit-identical transforms.

#include "physics/core/CharacterController.h"

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using psynder::Entity;
using psynder::f32;
using psynder::u32;
using psynder::math::Vec3;
using psynder::physics::CharacterController;
using psynder::physics::CharacterDesc;
using psynder::physics::CharacterInput;
using psynder::physics::character_input_system;
using psynder::physics::character_physics_step;
using psynder::physics::spawn_character;
using psynder::scene::PropDesc;
using psynder::scene::ShapeKind;
using psynder::scene::TransformWS;
using psynder::scene::World;
using psynder::scene::spawn_prop;

namespace {

constexpr f32 kDt = 1.0f / 120.0f;
constexpr Vec3 kGravity{0.0f, -9.81f, 0.0f};

// A wide static ground box whose top face sits at y == 0.
Entity add_ground(World& w) {
    PropDesc g;
    g.position = {0.0f, -0.5f, 0.0f};
    g.shape = ShapeKind::Box;
    g.half_extents = {50.0f, 0.5f, 50.0f};
    return spawn_prop(w, g);
}

void set_input(World& w, Entity e, f32 dx, f32 dz, f32 speed, bool jump) {
    auto* in = w.get<CharacterInput>(e);
    in->move_dir_x = dx;
    in->move_dir_z = dz;
    in->speed_mps = speed;
    in->jump = jump ? 1u : 0u;
}

void tick(World& w, f32 dt = kDt) {
    character_input_system(w);
    character_physics_step(w, dt, kGravity);
}

}  // namespace

TEST_CASE("character: gravity settles the capsule onto the ground",
          "[physics][ecs][character]") {
    World w;
    add_ground(w);
    CharacterDesc d;
    d.foot_position = {0.0f, 2.0f, 0.0f};
    const Entity e = spawn_character(w, d);

    for (int i = 0; i < 300; ++i) tick(w);

    const TransformWS* xf = w.get<TransformWS>(e);
    REQUIRE(xf != nullptr);
    const f32 foot_y = xf->mtw.m[13];
    REQUIRE(foot_y > -0.02f);
    REQUIRE(foot_y < 0.02f);
    REQUIRE(w.get<CharacterController>(e)->grounded == 1u);
}

TEST_CASE("character: horizontal input walks the capsule",
          "[physics][ecs][character]") {
    World w;
    add_ground(w);
    CharacterDesc d;
    d.foot_position = {0.0f, 0.5f, 0.0f};
    const Entity e = spawn_character(w, d);

    set_input(w, e, 1.0f, 0.0f, 4.0f, false);  // +X at 4 m/s
    for (int i = 0; i < 120; ++i) tick(w);       // ~1 s

    const f32 foot_x = w.get<TransformWS>(e)->mtw.m[12];
    REQUIRE(foot_x > 3.0f);   // moved roughly 4 m
    REQUIRE(foot_x < 5.0f);
}

TEST_CASE("character: a static box blocks horizontal motion",
          "[physics][ecs][character]") {
    World w;
    add_ground(w);

    PropDesc wall;
    wall.position = {2.0f, 0.5f, 0.0f};
    wall.shape = ShapeKind::Box;
    wall.half_extents = {0.5f, 1.0f, 5.0f};
    spawn_prop(w, wall);

    CharacterDesc d;
    d.foot_position = {0.0f, 0.5f, 0.0f};
    const Entity e = spawn_character(w, d);

    set_input(w, e, 1.0f, 0.0f, 4.0f, false);
    for (int i = 0; i < 240; ++i) tick(w);

    // Wall near face is at x == 1.5; capsule radius 0.38 → centre stops near
    // 1.12. It must not have tunnelled past the wall.
    const f32 foot_x = w.get<TransformWS>(e)->mtw.m[12];
    REQUIRE(foot_x < 1.2f);
}

TEST_CASE("character: jump launches the capsule upward",
          "[physics][ecs][character]") {
    World w;
    add_ground(w);
    CharacterDesc d;
    d.foot_position = {0.0f, 0.5f, 0.0f};
    d.jump_speed = 4.5f;
    const Entity e = spawn_character(w, d);

    for (int i = 0; i < 120; ++i) tick(w);   // settle + become grounded
    REQUIRE(w.get<CharacterController>(e)->grounded == 1u);

    set_input(w, e, 0.0f, 0.0f, 0.0f, true); // request jump
    tick(w);

    REQUIRE(w.get<CharacterController>(e)->velocity.y > 0.0f);
}

TEST_CASE("character: two identical worlds tick to bit-identical transforms",
          "[physics][ecs][character][determinism]") {
    auto build = [](World& w) {
        add_ground(w);
        CharacterDesc d;
        d.foot_position = {0.3f, 3.0f, -0.7f};
        return spawn_character(w, d);
    };

    World a, b;
    const Entity ea = build(a);
    const Entity eb = build(b);

    // Identical scripted input each tick (a little walk, then a jump).
    for (int i = 0; i < 400; ++i) {
        const bool jump = (i == 200);
        set_input(a, ea, 1.0f, 0.5f, 5.0f, jump);
        set_input(b, eb, 1.0f, 0.5f, 5.0f, jump);
        tick(a);
        tick(b);
    }

    const TransformWS* xa = a.get<TransformWS>(ea);
    const TransformWS* xb = b.get<TransformWS>(eb);
    REQUIRE(xa != nullptr);
    REQUIRE(xb != nullptr);
    for (int i = 0; i < 16; ++i) {
        REQUIRE(xa->mtw.m[i] == xb->mtw.m[i]);
    }
    const CharacterController* ca = a.get<CharacterController>(ea);
    const CharacterController* cb = b.get<CharacterController>(eb);
    REQUIRE(ca->velocity.x == cb->velocity.x);
    REQUIRE(ca->velocity.y == cb->velocity.y);
    REQUIRE(ca->velocity.z == cb->velocity.z);
    REQUIRE(ca->grounded == cb->grounded);
}
