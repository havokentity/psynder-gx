// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/physics_core_waveb_controller.cpp
//
// Lane 15 (physics-core) — Wave B (M2/M3) kinematic capsule character
// controller + solver-tuning tests. Complements physics_core_smoke.cpp
// (which covers M0/M1 rigid-body + basic-input acceptance).
//
// Smoke target from the lane brief:
//   - the capsule walks up a ramp within the slope limit,
//   - steps a curb within the step offset,
//   - is blocked by a wall steeper than the slope limit.
// Plus: ground-snap keeps it glued to the floor down a small drop,
// crouch/prone shrink the capsule, and the internal narrowphase / island /
// character tuning knobs (PhysicsTuning.h) round-trip.
//
// Assertions are behavioural thresholds (climbed > X, blocked < Y), not
// bit-exact positions, so they hold across the CI matrix (Jolt is built
// CROSS_PLATFORM_DETERMINISTIC, but arm64 vs x86_64 still differ in the
// low bits). The bit-exact local-determinism check lives in
// physics_core_smoke.cpp.

#include "physics/core/PublicPhysicsCore.h"
#include "physics/core/PhysicsTuning.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder::physics;

namespace {

constexpr float kDt    = 1.0f / 120.0f; // design fixed tick
constexpr float kPi    = 3.14159265358979323846f;

bool approx_eq(float a, float b, float tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

// Small world: the production default (64k bodies) over-reserves Jolt's
// 16 MiB temp allocator on the first tick. Unit scenes use a handful.
WorldDesc unit_world_desc() {
    WorldDesc d{};
    d.max_bodies      = 256u;
    d.max_constraints = 256u;
    d.tick_hz         = 120u;
    return d;
}

struct WorldScope {
    World* w = nullptr;
    std::vector<RigidBody*> bodies;
    WorldScope() {
        w = create_world(unit_world_desc());
        REQUIRE(w != nullptr);
    }
    ~WorldScope() {
        for (RigidBody* b : bodies) destroy_body(w, b);
        destroy_world(w);
    }
    // Static (mass 0) box, tracked so the scope tears it down on exit.
    // Half-extents in metres; quaternion {x,y,z,w}.
    RigidBody* box(float cx, float cy, float cz,
                   float hx, float hy, float hz,
                   float qx = 0.0f, float qy = 0.0f, float qz = 0.0f, float qw = 1.0f) {
        BodyDesc d{};
        d.shape    = Shape::Box;
        d.pos[0]   = cx; d.pos[1] = cy; d.pos[2] = cz;
        d.rot_quat[0] = qx; d.rot_quat[1] = qy; d.rot_quat[2] = qz; d.rot_quat[3] = qw;
        d.mass_kg  = 0.0f; // static
        d.dims[0]  = hx; d.dims[1] = hy; d.dims[2] = hz;
        RigidBody* b = create_body(w, d);
        REQUIRE(b != nullptr);
        bodies.push_back(b);
        return b;
    }
};

CharacterController* spawn_character(World* w, float x, float y, float z) {
    CharacterDesc cd{};
    cd.pos[0] = x; cd.pos[1] = y; cd.pos[2] = z;
    CharacterController* cc = create_character(w, cd);
    REQUIRE(cc != nullptr);
    return cc;
}

// Advance the character (and world) with a fixed planar input for N ticks.
void run_input(World* w, CharacterController* cc,
               float mx, float mz, int ticks,
               bool jump = false, bool crouch = false, bool prone = false) {
    CharacterInput in{};
    in.move_xy[0] = mx;
    in.move_xy[1] = mz;
    in.jump   = jump;
    in.crouch = crouch;
    in.prone  = prone;
    for (int i = 0; i < ticks; ++i) {
        character_tick(cc, in, kDt);
        tick(w, kDt);
    }
}

void settle(World* w, CharacterController* cc, int ticks) {
    run_input(w, cc, 0.0f, 0.0f, ticks);
}

} // namespace

// ─── Smoke target: ramp / curb / wall ────────────────────────────────────

TEST_CASE("physics-core wave-b: capsule climbs a ramp within the slope limit",
          "[physics][character][waveb]") {
    WorldScope scope;

    // 30 deg ramp (well within the 45 deg default slope limit). A wide slab
    // rotated -30 deg about X so +Z is uphill; the box centre is chosen so
    // the top surface passes through the origin: y = tan(30 deg) * z.
    const float ramp_rad = 30.0f * kPi / 180.0f;
    const float cos_a = std::cos(ramp_rad);
    const float sin_a = std::sin(ramp_rad);
    const float tan_a = sin_a / cos_a;
    const float half  = 0.5f * ramp_rad;
    const float hy    = 0.5f;
    scope.box(0.0f, -hy * cos_a, hy * sin_a, // centre puts the top face on y=tan*z
              20.0f, hy, 15.0f,
              -std::sin(half), 0.0f, 0.0f, std::cos(half)); // rotate -30 deg about X

    const float z0 = -4.0f;
    CharacterController* cc =
        spawn_character(scope.w, 0.0f, tan_a * z0 + 0.1f, z0);

    settle(scope.w, cc, 90); // drop the 0.1 m and settle on the slope

    float p0[3]{};
    character_get_transform(cc, p0, nullptr);

    run_input(scope.w, cc, 0.0f, 1.0f, 120); // drive uphill (+Z) for 1 s

    float p1[3]{};
    character_get_transform(cc, p1, nullptr);

    REQUIRE(p1[2] > p0[2] + 0.3f); // advanced up the ramp
    REQUIRE(p1[1] > p0[1] + 0.3f); // gained height

    destroy_character(scope.w, cc);
}

TEST_CASE("physics-core wave-b: capsule steps up a curb within the step offset",
          "[physics][character][waveb]") {
    WorldScope scope;

    // Flat floor (top at y=0) + a 0.20 m curb (< 0.30 m default step offset)
    // occupying z >= 0 with its front face at z=0.
    scope.box(0.0f, -0.5f, 0.0f, 20.0f, 0.5f, 20.0f);          // floor
    scope.box(0.0f,  0.1f, 10.0f, 20.0f, 0.1f, 10.0f);        // curb top y=0.2

    CharacterController* cc = spawn_character(scope.w, 0.0f, 0.1f, -2.0f);
    settle(scope.w, cc, 60);

    float p0[3]{};
    character_get_transform(cc, p0, nullptr);

    run_input(scope.w, cc, 0.0f, 1.0f, 180); // walk into and over the curb

    float p1[3]{};
    character_get_transform(cc, p1, nullptr);

    REQUIRE(p1[1] > p0[1] + 0.1f); // stepped up onto the ~0.2 m curb
    REQUIRE(p1[2] > 0.3f);         // got past the curb's front edge (z=0)

    destroy_character(scope.w, cc);
}

TEST_CASE("physics-core wave-b: capsule is blocked by a curb taller than the step offset",
          "[physics][character][waveb]") {
    WorldScope scope;

    // 0.60 m curb (> 0.30 m step offset): the controller can't step it.
    scope.box(0.0f, -0.5f, 0.0f, 20.0f, 0.5f, 20.0f);          // floor
    scope.box(0.0f,  0.3f, 10.0f, 20.0f, 0.3f, 10.0f);        // curb top y=0.6

    CharacterController* cc = spawn_character(scope.w, 0.0f, 0.1f, -2.0f);
    settle(scope.w, cc, 60);

    run_input(scope.w, cc, 0.0f, 1.0f, 180);

    float p1[3]{};
    character_get_transform(cc, p1, nullptr);

    REQUIRE(p1[1] < 0.3f);  // did not climb onto the 0.6 m curb top
    REQUIRE(p1[2] < 0.0f);  // stopped in front of the curb (front face at z=0)
    REQUIRE(p1[2] > -1.5f); // but did advance from the z=-2 start toward it

    destroy_character(scope.w, cc);
}

TEST_CASE("physics-core wave-b: capsule is blocked by a wall steeper than the slope limit",
          "[physics][character][waveb]") {
    WorldScope scope;

    // Vertical wall (90 deg, far above the 45 deg slope limit), front face
    // at z=-0.5, on a flat floor.
    scope.box(0.0f, -0.5f, 0.0f, 20.0f, 0.5f, 20.0f);          // floor
    scope.box(0.0f,  2.0f, 0.0f, 20.0f, 2.0f, 0.5f);          // wall y[0,4] z[-0.5,0.5]

    CharacterController* cc = spawn_character(scope.w, 0.0f, 0.1f, -3.0f);
    settle(scope.w, cc, 60);

    run_input(scope.w, cc, 0.0f, 1.0f, 180);

    float p1[3]{};
    character_get_transform(cc, p1, nullptr);

    REQUIRE(p1[2] < -0.4f); // stopped before the wall (front face at z=-0.5)
    REQUIRE(p1[2] > -2.0f); // but advanced from z=-3 up to the wall
    REQUIRE(p1[1] < 0.5f);  // did not climb the wall

    destroy_character(scope.w, cc);
}

// ─── Ground snap ──────────────────────────────────────────────────────────

TEST_CASE("physics-core wave-b: ground snap keeps the capsule grounded over a small drop",
          "[physics][character][waveb]") {
    WorldScope scope;

    // Upper floor (top y=0) for z in [-20, 0]; lower floor (top y=-0.2) for
    // z in [0, 20]. The 0.20 m drop is within the 0.30 m ground-snap range.
    scope.box(0.0f, -0.5f, -10.0f, 20.0f, 0.5f, 10.0f); // upper, top y=0
    scope.box(0.0f, -0.7f,  10.0f, 20.0f, 0.5f, 10.0f); // lower, top y=-0.2

    CharacterController* cc = spawn_character(scope.w, 0.0f, 0.1f, -3.0f);
    settle(scope.w, cc, 60);
    REQUIRE(character_ground_state(cc) == CharacterGroundState::OnGround);

    run_input(scope.w, cc, 0.0f, 1.0f, 120); // walk across the lip onto the lower slab
    settle(scope.w, cc, 10);                 // let the read stabilise

    float p1[3]{};
    character_get_transform(cc, p1, nullptr);

    // Without ground snap the capsule would launch off the lip at speed and
    // be airborne here; with it, StickToFloor keeps it planted.
    REQUIRE(character_ground_state(cc) == CharacterGroundState::OnGround);
    REQUIRE(p1[2] > 0.5f); // crossed onto the lower slab

    destroy_character(scope.w, cc);
}

// ─── Stance ────────────────────────────────────────────────────────────────

TEST_CASE("physics-core wave-b: crouch and prone shrink the capsule, standing restores it",
          "[physics][character][waveb]") {
    WorldScope scope;
    scope.box(0.0f, -0.5f, 0.0f, 20.0f, 0.5f, 20.0f); // floor, no ceiling

    CharacterController* cc = spawn_character(scope.w, 0.0f, 0.1f, 0.0f);
    settle(scope.w, cc, 60);

    const float stand_h = character_capsule_height_m(cc);
    REQUIRE(character_stance(cc) == CharacterStance::Stand);
    REQUIRE(approx_eq(stand_h, 1.8f)); // public CharacterDesc default height

    run_input(scope.w, cc, 0.0f, 0.0f, 30, /*jump=*/false, /*crouch=*/true);
    REQUIRE(character_stance(cc) == CharacterStance::Crouch);
    const float crouch_h = character_capsule_height_m(cc);
    REQUIRE(crouch_h < stand_h);

    run_input(scope.w, cc, 0.0f, 0.0f, 30, /*jump=*/false, /*crouch=*/false, /*prone=*/true);
    REQUIRE(character_stance(cc) == CharacterStance::Prone);
    REQUIRE(character_capsule_height_m(cc) < crouch_h);

    settle(scope.w, cc, 30); // release: stand back up (open headroom)
    REQUIRE(character_stance(cc) == CharacterStance::Stand);
    REQUIRE(approx_eq(character_capsule_height_m(cc), stand_h));

    destroy_character(scope.w, cc);
}

// ─── Lean ────────────────────────────────────────────────────────────────

TEST_CASE("physics-core wave-b: lean eases toward the input and clamps",
          "[physics][character][waveb]") {
    WorldScope scope;
    scope.box(0.0f, -0.5f, 0.0f, 20.0f, 0.5f, 20.0f);

    CharacterController* cc = spawn_character(scope.w, 0.0f, 0.1f, 0.0f);
    settle(scope.w, cc, 60);
    REQUIRE(approx_eq(character_lean(cc), 0.0f));

    // Hold lean-right: eases toward +1 and clamps there.
    CharacterInput right{};
    right.lean_right = true;
    for (int i = 0; i < 60; ++i) { character_tick(cc, right, kDt); tick(scope.w, kDt); }
    const float leaned = character_lean(cc);
    REQUIRE(leaned > 0.5f);
    REQUIRE(leaned <= 1.0f + 1e-4f);

    // Release: eases back toward neutral.
    settle(scope.w, cc, 60);
    REQUIRE(character_lean(cc) < leaned);
    REQUIRE(approx_eq(character_lean(cc), 0.0f, 0.05f));

    destroy_character(scope.w, cc);
}

// ─── (b) Narrowphase + island solver tuning knobs ────────────────────────

TEST_CASE("physics-core wave-b: narrowphase and island tuning round-trip",
          "[physics][tuning][waveb]") {
    WorldScope scope;

    SECTION("narrowphase config") {
        const NarrowphaseConfig def = narrowphase_config(scope.w);
        REQUIRE(approx_eq(def.speculative_contact_distance_m, 0.02f));
        REQUIRE(approx_eq(def.penetration_slop_m, 0.02f));

        NarrowphaseConfig c{};
        c.speculative_contact_distance_m = 0.05f;
        c.penetration_slop_m             = 0.015f;
        c.manifold_tolerance_m           = 2.0e-3f;
        c.max_penetration_distance_m     = 0.25f;
        c.linear_cast_threshold          = 0.6f;
        c.linear_cast_max_penetration    = 0.3f;
        c.use_manifold_reduction         = false;
        c.use_body_pair_cache            = false;
        c.check_active_edges             = false;
        set_narrowphase_config(scope.w, c);

        const NarrowphaseConfig got = narrowphase_config(scope.w);
        REQUIRE(approx_eq(got.speculative_contact_distance_m, 0.05f));
        REQUIRE(approx_eq(got.penetration_slop_m, 0.015f));
        REQUIRE(approx_eq(got.manifold_tolerance_m, 2.0e-3f));
        REQUIRE(approx_eq(got.max_penetration_distance_m, 0.25f));
        REQUIRE(approx_eq(got.linear_cast_threshold, 0.6f));
        REQUIRE(approx_eq(got.linear_cast_max_penetration, 0.3f));
        REQUIRE(got.use_manifold_reduction == false);
        REQUIRE(got.use_body_pair_cache == false);
        REQUIRE(got.check_active_edges == false);

        // Touching narrowphase must not disturb the island/solver group.
        const IslandSolverConfig isl = island_solver_config(scope.w);
        REQUIRE(isl.velocity_steps == 10u);
        REQUIRE(isl.position_steps == 2u);
    }

    SECTION("island solver config") {
        const IslandSolverConfig def = island_solver_config(scope.w);
        REQUIRE(def.velocity_steps == 10u);
        REQUIRE(def.position_steps == 2u);
        REQUIRE(approx_eq(def.baumgarte, 0.2f));

        IslandSolverConfig c{};
        c.velocity_steps                       = 16u;
        c.position_steps                       = 4u;
        c.baumgarte                            = 0.25f;
        c.min_velocity_for_restitution_mps     = 1.5f;
        c.time_before_sleep_s                  = 0.75f;
        c.point_velocity_sleep_threshold_mps   = 0.05f;
        c.use_large_island_splitter            = false;
        c.constraint_warm_start                = false;
        c.allow_sleeping                       = false;
        c.deterministic                        = true;
        set_island_solver_config(scope.w, c);

        const IslandSolverConfig got = island_solver_config(scope.w);
        REQUIRE(got.velocity_steps == 16u);
        REQUIRE(got.position_steps == 4u);
        REQUIRE(approx_eq(got.baumgarte, 0.25f));
        REQUIRE(approx_eq(got.min_velocity_for_restitution_mps, 1.5f));
        REQUIRE(approx_eq(got.time_before_sleep_s, 0.75f));
        REQUIRE(approx_eq(got.point_velocity_sleep_threshold_mps, 0.05f));
        REQUIRE(got.use_large_island_splitter == false);
        REQUIRE(got.constraint_warm_start == false);
        REQUIRE(got.allow_sleeping == false);
        REQUIRE(got.deterministic == true);

        // Independent of the narrowphase group.
        const NarrowphaseConfig np = narrowphase_config(scope.w);
        REQUIRE(approx_eq(np.speculative_contact_distance_m, 0.02f));

        // Degenerate values are clamped: friction needs >= 2 velocity
        // iterations, and baumgarte is a 0..1 fraction.
        IslandSolverConfig bad{};
        bad.velocity_steps = 1u;   // below the friction floor
        bad.baumgarte      = 1.5f; // above the valid range
        set_island_solver_config(scope.w, bad);
        const IslandSolverConfig clamped = island_solver_config(scope.w);
        REQUIRE(clamped.velocity_steps == 2u);
        REQUIRE(approx_eq(clamped.baumgarte, 1.0f));

        IslandSolverConfig neg{};
        neg.baumgarte = -0.5f; // below the valid range
        set_island_solver_config(scope.w, neg);
        REQUIRE(approx_eq(island_solver_config(scope.w).baumgarte, 0.0f));
    }
}

TEST_CASE("physics-core wave-b: character tuning round-trips",
          "[physics][tuning][waveb]") {
    WorldScope scope;
    scope.box(0.0f, -0.5f, 0.0f, 20.0f, 0.5f, 20.0f);
    CharacterController* cc = spawn_character(scope.w, 0.0f, 0.1f, 0.0f);

    const CharacterTuning def = character_tuning(cc);
    REQUIRE(approx_eq(def.max_slope_angle_deg, 45.0f));
    REQUIRE(approx_eq(def.step_offset_m, 0.30f));
    REQUIRE(approx_eq(def.ground_snap_dist_m, 0.30f));
    REQUIRE(approx_eq(def.mass_kg, 80.0f));
    REQUIRE(approx_eq(def.max_push_strength_n, 200.0f));

    CharacterTuning t = def;
    t.max_slope_angle_deg = 50.0f;
    t.step_offset_m       = 0.45f;
    t.ground_snap_dist_m  = 0.50f;
    t.mass_kg             = 90.0f;
    t.max_push_strength_n = 300.0f;
    t.lean_offset_m       = 0.30f;
    t.lean_speed_mps      = 4.0f;
    set_character_tuning(cc, t);

    const CharacterTuning got = character_tuning(cc);
    REQUIRE(approx_eq(got.max_slope_angle_deg, 50.0f));
    REQUIRE(approx_eq(got.step_offset_m, 0.45f));
    REQUIRE(approx_eq(got.ground_snap_dist_m, 0.50f));
    REQUIRE(approx_eq(got.mass_kg, 90.0f));
    REQUIRE(approx_eq(got.max_push_strength_n, 300.0f));
    REQUIRE(approx_eq(got.lean_offset_m, 0.30f));
    REQUIRE(approx_eq(got.lean_speed_mps, 4.0f));

    destroy_character(scope.w, cc);
}
