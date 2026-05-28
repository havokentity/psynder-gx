// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/physics_core_character_spine.cpp

#include "physics/core/CharacterSpine.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace spine = psynder::physics::character_spine;

namespace {

constexpr int kOneSecondTicks = 120;

bool approx(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

struct SpineScope {
    spine::World* world = nullptr;

    SpineScope() {
        spine::WorldDesc desc{};
        desc.max_bodies = 64;
        desc.tick_hz = 120;
        world = spine::create_world(desc);
        REQUIRE(world != nullptr);
    }

    ~SpineScope() {
        spine::destroy_world(world);
    }
};

void settle(spine::World* world, int ticks = 60) {
    for (int i = 0; i < ticks; ++i) {
        spine::step_fixed(world);
    }
}

} // namespace

TEST_CASE("physics-core character spine: desired velocity moves at fixed rate",
          "[physics][character][spine]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[1] = 0.1f;
    desc.max_horizontal_speed_mps = 3.0f;
    spine::Character* character =
        spine::create_capsule_character(scope.world, desc);
    REQUIRE(character != nullptr);

    settle(scope.world);
    const spine::Transform start = spine::character_transform(character);

    spine::set_desired_horizontal_velocity(character, 0.0f, 1.5f);
    for (int i = 0; i < kOneSecondTicks; ++i) {
        spine::step_fixed(scope.world);
    }

    const spine::Transform end = spine::character_transform(character);
    REQUIRE(approx(end.foot_position_m[0], start.foot_position_m[0], 0.05f));
    REQUIRE(approx(end.foot_position_m[2] - start.foot_position_m[2],
                   1.5f,
                   0.15f));
    REQUIRE(approx(end.foot_position_m[1], start.foot_position_m[1], 0.05f));
}

TEST_CASE("physics-core character spine: static box blocks capsule motion",
          "[physics][character][spine]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::BoxDesc wall{};
    wall.center_m[1] = 1.0f;
    wall.center_m[2] = 1.125f;
    wall.half_extents_m[0] = 4.0f;
    wall.half_extents_m[1] = 1.0f;
    wall.half_extents_m[2] = 0.125f; // front face at z = 1.0 m
    REQUIRE(spine::add_static_box(scope.world, wall));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[1] = 0.1f;
    desc.max_horizontal_speed_mps = 3.0f;
    spine::Character* character =
        spine::create_capsule_character(scope.world, desc);
    REQUIRE(character != nullptr);

    settle(scope.world);

    spine::set_desired_horizontal_velocity(character, 0.0f, 3.0f);
    for (int i = 0; i < kOneSecondTicks; ++i) {
        spine::step_fixed(scope.world);
    }

    const spine::Transform end = spine::character_transform(character);
    REQUIRE(end.foot_position_m[2] > 0.25f);
    REQUIRE(end.foot_position_m[2] < 0.75f);
    REQUIRE(end.foot_position_m[1] < 0.10f);
}

TEST_CASE("physics-core character spine: static sphere blocks capsule motion",
          "[physics][character][spine]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::SphereDesc invalid_sphere{};
    invalid_sphere.radius_m = 0.0f;
    REQUIRE_FALSE(spine::add_static_sphere(scope.world, invalid_sphere));

    spine::SphereDesc blocker{};
    blocker.center_m[1] = 0.45f;
    blocker.center_m[2] = 1.25f;
    blocker.radius_m = 0.45f;
    REQUIRE(spine::add_static_sphere(scope.world, blocker));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[1] = 0.1f;
    desc.max_horizontal_speed_mps = 3.0f;
    spine::Character* character =
        spine::create_capsule_character(scope.world, desc);
    REQUIRE(character != nullptr);

    settle(scope.world);

    spine::set_desired_horizontal_velocity(character, 0.0f, 3.0f);
    for (int i = 0; i < kOneSecondTicks; ++i) {
        spine::step_fixed(scope.world);
    }

    const spine::Transform end = spine::character_transform(character);
    REQUIRE(end.foot_position_m[2] > 0.20f);
    REQUIRE(end.foot_position_m[2] < 0.95f);
    REQUIRE(end.foot_position_m[1] < 0.10f);
}

TEST_CASE("physics-core character spine: static capsule blocks capsule motion",
          "[physics][character][spine]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::StaticCapsuleDesc invalid_capsule{};
    invalid_capsule.height_m = 0.7f;
    invalid_capsule.radius_m = 0.4f;
    REQUIRE_FALSE(spine::add_static_capsule(scope.world, invalid_capsule));

    spine::StaticCapsuleDesc blocker{};
    blocker.center_m[1] = 0.9f;
    blocker.center_m[2] = 1.25f;
    blocker.radius_m = 0.40f;
    blocker.height_m = 1.80f;
    REQUIRE(spine::add_static_capsule(scope.world, blocker));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[1] = 0.1f;
    desc.max_horizontal_speed_mps = 3.0f;
    spine::Character* character =
        spine::create_capsule_character(scope.world, desc);
    REQUIRE(character != nullptr);

    settle(scope.world);

    spine::set_desired_horizontal_velocity(character, 0.0f, 3.0f);
    for (int i = 0; i < kOneSecondTicks; ++i) {
        spine::step_fixed(scope.world);
    }

    const spine::Transform end = spine::character_transform(character);
    REQUIRE(end.foot_position_m[2] > 0.20f);
    REQUIRE(end.foot_position_m[2] < 0.95f);
    REQUIRE(end.foot_position_m[1] < 0.10f);
}

TEST_CASE("physics-core character spine: character input clamps horizontal speed",
          "[physics][character][spine]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[1] = 0.1f;
    desc.max_horizontal_speed_mps = 2.0f;
    spine::Character* character =
        spine::create_capsule_character(scope.world, desc);
    REQUIRE(character != nullptr);

    settle(scope.world);
    const spine::Transform start = spine::character_transform(character);

    spine::CharacterInput input{};
    input.velocity_mps[0] = 12.0f;
    input.velocity_mps[1] = 16.0f;
    spine::set_character_input(character, input);
    for (int i = 0; i < kOneSecondTicks; ++i) {
        spine::step_fixed(scope.world);
    }

    const spine::Transform end = spine::character_transform(character);
    const float dx = end.foot_position_m[0] - start.foot_position_m[0];
    const float dz = end.foot_position_m[2] - start.foot_position_m[2];
    const float distance = std::sqrt(dx * dx + dz * dz);

    REQUIRE(approx(distance, 2.0f, 0.20f));
    REQUIRE(approx(dx / distance, 0.6f, 0.05f));
    REQUIRE(approx(dz / distance, 0.8f, 0.05f));
    REQUIRE(end.foot_position_m[1] < 0.10f);
}

TEST_CASE("physics-core character spine: desired velocity setter clamps speed",
          "[physics][character][spine]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[1] = 0.1f;
    desc.max_horizontal_speed_mps = 2.0f;
    spine::Character* character =
        spine::create_capsule_character(scope.world, desc);
    REQUIRE(character != nullptr);

    settle(scope.world);
    const spine::Transform start = spine::character_transform(character);

    spine::set_desired_horizontal_velocity(character, 8.0f, 0.0f);
    for (int i = 0; i < kOneSecondTicks; ++i) {
        spine::step_fixed(scope.world);
    }

    const spine::Transform end = spine::character_transform(character);
    REQUIRE(approx(end.foot_position_m[0] - start.foot_position_m[0],
                   2.0f,
                   0.20f));
    REQUIRE(approx(end.foot_position_m[2], start.foot_position_m[2], 0.05f));
    REQUIRE(end.foot_position_m[1] < 0.10f);
}

TEST_CASE("physics-core character spine: zero input does not drift on ground",
          "[physics][character][spine]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[0] = 1.25f;
    desc.foot_position_m[1] = 0.1f;
    desc.foot_position_m[2] = -0.75f;
    spine::Character* character =
        spine::create_capsule_character(scope.world, desc);
    REQUIRE(character != nullptr);

    settle(scope.world);
    const spine::Transform settled = spine::character_transform(character);

    spine::CharacterInput input{};
    spine::set_character_input(character, input);
    for (int i = 0; i < kOneSecondTicks * 3; ++i) {
        spine::step_fixed(scope.world);
    }

    const spine::Transform end = spine::character_transform(character);
    REQUIRE(approx(end.foot_position_m[0], settled.foot_position_m[0], 0.02f));
    REQUIRE(approx(end.foot_position_m[1], settled.foot_position_m[1], 0.03f));
    REQUIRE(approx(end.foot_position_m[2], settled.foot_position_m[2], 0.02f));
}

TEST_CASE("physics-core character spine: jump launches and lands",
          "[physics][character][spine]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[1] = 0.1f;
    spine::Character* character =
        spine::create_capsule_character(scope.world, desc);
    REQUIRE(character != nullptr);

    settle(scope.world);
    const spine::Transform grounded = spine::character_transform(character);

    spine::CharacterInput input{};
    input.jump = true;
    spine::set_character_input(character, input);
    spine::step_fixed(scope.world);

    input.jump = false;
    spine::set_character_input(character, input);

    float peak_y = grounded.foot_position_m[1];
    for (int i = 0; i < kOneSecondTicks / 2; ++i) {
        spine::step_fixed(scope.world);
        const spine::Transform current = spine::character_transform(character);
        if (current.foot_position_m[1] > peak_y) {
            peak_y = current.foot_position_m[1];
        }
    }

    REQUIRE(peak_y > grounded.foot_position_m[1] + 0.35f);

    for (int i = 0; i < kOneSecondTicks; ++i) {
        spine::step_fixed(scope.world);
    }

    const spine::Transform landed = spine::character_transform(character);
    REQUIRE(approx(landed.foot_position_m[1],
                   grounded.foot_position_m[1],
                   0.08f));
}

TEST_CASE("physics-core character spine: low ceiling blocks jump",
          "[physics][character][spine]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::BoxDesc ceiling{};
    ceiling.center_m[1] = 2.175f;
    ceiling.half_extents_m[0] = 4.0f;
    ceiling.half_extents_m[1] = 0.125f; // bottom face at y = 2.05 m
    ceiling.half_extents_m[2] = 4.0f;
    REQUIRE(spine::add_static_box(scope.world, ceiling));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[1] = 0.1f;
    spine::Character* character =
        spine::create_capsule_character(scope.world, desc);
    REQUIRE(character != nullptr);

    settle(scope.world);
    const spine::Transform grounded = spine::character_transform(character);

    spine::CharacterInput input{};
    input.jump = true;
    spine::set_character_input(character, input);
    spine::step_fixed(scope.world);

    input.jump = false;
    spine::set_character_input(character, input);

    float peak_y = grounded.foot_position_m[1];
    for (int i = 0; i < kOneSecondTicks; ++i) {
        spine::step_fixed(scope.world);
        const spine::Transform current = spine::character_transform(character);
        if (current.foot_position_m[1] > peak_y) {
            peak_y = current.foot_position_m[1];
        }
    }

    REQUIRE(peak_y < grounded.foot_position_m[1] + 0.35f);

    for (int i = 0; i < kOneSecondTicks; ++i) {
        spine::step_fixed(scope.world);
    }

    const spine::Transform landed = spine::character_transform(character);
    REQUIRE(approx(landed.foot_position_m[1],
                   grounded.foot_position_m[1],
                   0.08f));
}

TEST_CASE("physics-core character spine: fixed input stream is deterministic",
          "[physics][character][spine]") {
    SpineScope a;
    SpineScope b;
    REQUIRE(spine::add_static_ground(a.world, 12.0f, 12.0f));
    REQUIRE(spine::add_static_ground(b.world, 12.0f, 12.0f));

    spine::BoxDesc blocker{};
    blocker.center_m[0] = 0.75f;
    blocker.center_m[1] = 1.0f;
    blocker.center_m[2] = 1.75f;
    blocker.half_extents_m[0] = 0.25f;
    blocker.half_extents_m[1] = 1.0f;
    blocker.half_extents_m[2] = 0.25f;
    REQUIRE(spine::add_static_box(a.world, blocker));
    REQUIRE(spine::add_static_box(b.world, blocker));

    spine::CapsuleDesc desc{};
    desc.foot_position_m[1] = 0.1f;
    desc.max_horizontal_speed_mps = 5.0f;
    spine::Character* char_a =
        spine::create_capsule_character(a.world, desc);
    spine::Character* char_b =
        spine::create_capsule_character(b.world, desc);
    REQUIRE(char_a != nullptr);
    REQUIRE(char_b != nullptr);

    settle(a.world);
    settle(b.world);

    for (int tick = 0; tick < kOneSecondTicks * 2; ++tick) {
        spine::CharacterInput input{};
        input.velocity_mps[0] = (tick < 90) ? 3.0f : -1.5f;
        input.velocity_mps[1] = (tick < 160) ? 4.0f : 0.75f;
        input.jump = tick == 12;
        input.crouch = tick >= 96 && tick < 150;
        input.lean_right = tick >= 30 && tick < 70;

        spine::set_character_input(char_a, input);
        spine::set_character_input(char_b, input);
        spine::step_fixed(a.world);
        spine::step_fixed(b.world);
    }

    const spine::Transform end_a = spine::character_transform(char_a);
    const spine::Transform end_b = spine::character_transform(char_b);
    REQUIRE(approx(end_a.foot_position_m[0], end_b.foot_position_m[0], 1.0e-5f));
    REQUIRE(approx(end_a.foot_position_m[1], end_b.foot_position_m[1], 1.0e-5f));
    REQUIRE(approx(end_a.foot_position_m[2], end_b.foot_position_m[2], 1.0e-5f));
    REQUIRE(approx(end_a.yaw_deg, end_b.yaw_deg, 1.0e-5f));
}

TEST_CASE("physics-core character spine: dynamic body falls and settles on ground",
          "[physics][spine][dynamic]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::DynamicBoxDesc desc{};
    desc.center_m[1] = 3.0f;
    desc.half_extents_m[0] = 0.5f;
    desc.half_extents_m[1] = 0.5f;
    desc.half_extents_m[2] = 0.5f;
    desc.mass_kg = 8.0f;

    // Invalid descs are rejected (zero mass / zero extent).
    spine::DynamicBoxDesc invalid = desc;
    invalid.mass_kg = 0.0f;
    REQUIRE_FALSE(spine::add_dynamic_box(scope.world, invalid));

    const spine::DynamicBodyHandle handle = spine::add_dynamic_box(scope.world, desc);
    REQUIRE(handle.valid());

    float pos[3] = {0, 0, 0};
    float quat[4] = {0, 0, 0, 1};
    REQUIRE(spine::dynamic_body_transform(scope.world, handle, pos, quat));
    const float start_y = pos[1];

    for (int i = 0; i < 360; ++i) spine::step_fixed(scope.world);  // ~3 s to settle

    REQUIRE(spine::dynamic_body_transform(scope.world, handle, pos, quat));
    REQUIRE(pos[1] < start_y - 1.0f);  // it fell
    REQUIRE(pos[1] > 0.2f);            // rests near 0.5 (half-extent on the ground)
    REQUIRE(pos[1] < 0.9f);
}

TEST_CASE("physics-core character spine: impulse moves a dynamic body; remove drops it",
          "[physics][spine][dynamic]") {
    SpineScope scope;
    REQUIRE(spine::add_static_ground(scope.world, 12.0f, 12.0f));

    spine::DynamicBoxDesc desc{};
    desc.center_m[1] = 0.5f;  // resting on the ground
    desc.mass_kg = 6.0f;
    const spine::DynamicBodyHandle handle = spine::add_dynamic_box(scope.world, desc);
    REQUIRE(handle.valid());

    settle(scope.world, 30);

    float pos[3] = {0, 0, 0};
    float quat[4] = {0, 0, 0, 1};
    REQUIRE(spine::dynamic_body_transform(scope.world, handle, pos, quat));
    const float start_x = pos[0];

    spine::dynamic_body_apply_impulse(scope.world, handle, 30.0f, 0.0f, 0.0f);  // kick +X
    for (int i = 0; i < 60; ++i) spine::step_fixed(scope.world);

    REQUIRE(spine::dynamic_body_transform(scope.world, handle, pos, quat));
    REQUIRE(pos[0] > start_x + 0.3f);  // the impulse pushed it along +X

    // Removing it succeeds once, then reports gone; an invalid handle is false.
    REQUIRE(spine::remove_dynamic_body(scope.world, handle));
    REQUIRE_FALSE(spine::remove_dynamic_body(scope.world, handle));
    REQUIRE_FALSE(spine::remove_dynamic_body(scope.world, spine::DynamicBodyHandle{}));
    REQUIRE_FALSE(spine::dynamic_body_transform(scope.world, handle, pos, quat));
}
