// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/physics_vehicle_waveb_smoke.cpp
//
// Lane 16 (Wave B/C) — vehicle drivetrain + Pacejka-lite tire model smoke +
// unit tests. Two tiers:
//   1. Pure-function tests on VehicleModel.h (Pacejka, friction circle, engine
//      curve, aero, suspension, quaternion helpers) — deterministic math.
//   2. Integration smoke through the PublicVehicle.h API: the car accelerates,
//      a redline-limited first gear is beaten by upshifting through the box,
//      braking sheds speed, reverse drives backward, hard steer + throttle
//      breaks traction into a slide, and identical inputs replay bit-exactly.
//
// TEST_CASE names are ASCII-only (AGENTS.md: ctest re-feeds names as filters
// and a non-ASCII name is mangled by the Windows CRT argv decode).
//
// The vehicle sim is self-contained (see Vehicle.cpp), so create_vehicle is
// given a null World* here — it stores the pointer but never dereferences it.

#include "physics/core/PublicPhysicsCore.h"
#include "physics/vehicle/PublicVehicle.h"
#include "physics/vehicle/VehicleModel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace psynder::physics::vehicle;
using Catch::Approx;

namespace {

constexpr float kDt = 1.0f / 120.0f;

// A realistic rear-wheel-drive sedan. Arrays live on the stack for the
// duration of create_vehicle, which copies them.
struct CarRig {
    WheelDesc wheels[4]{};
    EngineCurvePoint curve[6]{};
    float gears[7]{};
    VehicleDesc desc{};

    CarRig() {
        const float half_track = 0.80f;
        const float front_z = 1.40f;
        const float rear_z = -1.40f;
        for (int i = 0; i < 4; ++i) {
            WheelDesc& w = wheels[i];
            w.local_pos[0] = (i % 2 == 0) ? -half_track : half_track;  // L / R
            w.local_pos[1] = -0.30f;                                    // hub below center
            w.local_pos[2] = (i < 2) ? front_z : rear_z;                // front / rear
            w.suspension_rest_m = 0.30f;
            w.suspension_stiffness = 45000.0f;  // ~6.8 cm static deflection at this mass
            w.suspension_damping = 4000.0f;
            w.tire_radius_m = 0.34f;
            w.tire_width_m = 0.22f;
            w.tire_friction_mu = 1.05f;  // dry tarmac
            w.is_driven = (i >= 2);      // RWD: rear wheels driven
            w.is_steered = (i < 2);      // front wheels steer
            w.is_braked = true;
        }
        curve[0] = {1000.0f, 180.0f};
        curve[1] = {2000.0f, 240.0f};
        curve[2] = {3500.0f, 300.0f};
        curve[3] = {5000.0f, 320.0f};
        curve[4] = {6000.0f, 300.0f};
        curve[5] = {7000.0f, 250.0f};
        // {reverse, neutral, 1st .. 5th}
        gears[0] = -3.20f;
        gears[1] = 0.0f;
        gears[2] = 3.20f;
        gears[3] = 2.10f;
        gears[4] = 1.50f;
        gears[5] = 1.10f;
        gears[6] = 0.85f;

        desc.mass_kg = 1250.0f;
        desc.drag_cd = 0.30f;
        desc.frontal_area_m2 = 2.20f;
        desc.center_of_mass_offset_z_m = -0.10f;
        desc.drivetrain = Drivetrain::Rwd;
        desc.wheel_count = 4;
        desc.wheels = wheels;
        desc.engine_curve_n = 6;
        desc.engine_curve = curve;
        desc.gear_count = 7;
        desc.gear_ratios = gears;
        desc.final_drive = 3.90f;
    }
};

void settle(Vehicle* v, int ticks = 180) {
    VehicleInput idle{};
    for (int i = 0; i < ticks; ++i) vehicle_tick(v, idle, kDt);
}

}  // namespace

// ─── Tier 1: pure-function math ───────────────────────────────────────────────

TEST_CASE("vehicle: pacejka is zero at zero, odd, and bounded by the peak",
          "[vehicle][tire]") {
    const float d = 1.05f;
    REQUIRE(pacejka(kPacejkaLatB, kPacejkaLatC, d, kPacejkaLatE, 0.0f) == Approx(0.0f));
    for (float s : {0.02f, 0.10f, 0.30f}) {
        const float pos = pacejka(kPacejkaLatB, kPacejkaLatC, d, kPacejkaLatE, s);
        const float neg = pacejka(kPacejkaLatB, kPacejkaLatC, d, kPacejkaLatE, -s);
        REQUIRE(pos == Approx(-neg).margin(1e-5f));
        REQUIRE(std::fabs(pos) <= d + 1e-4f);
    }
}

TEST_CASE("vehicle: tire grip rises from zero and saturates near mu",
          "[vehicle][tire]") {
    const float mu = 1.0f;
    REQUIRE(tire_mu_long(0.0f, mu) == Approx(0.0f));
    REQUIRE(tire_mu_long(0.02f, mu) > 0.0f);
    float peak = 0.0f;
    for (int i = 1; i <= 200; ++i) peak = std::max(peak, tire_mu_long(i * 0.01f, mu));
    REQUIRE(peak > 0.9f * mu);
    REQUIRE(peak <= mu + 1e-4f);
}

TEST_CASE("vehicle: friction circle caps the combined tire force",
          "[vehicle][tire]") {
    const float fz = 4000.0f;
    const float mu = 1.0f;
    const float limit = mu * fz;

    float fx = limit;
    float fy = limit;  // each axis demanding full grip -> sqrt(2)x over the circle
    clamp_friction_circle(fx, fy, limit);
    REQUIRE(std::sqrt(fx * fx + fy * fy) == Approx(limit).margin(1.0f));

    float gx = 0.30f * limit;
    float gy = 0.20f * limit;
    const float inside = std::sqrt(gx * gx + gy * gy);
    clamp_friction_circle(gx, gy, limit);
    REQUIRE(std::sqrt(gx * gx + gy * gy) == Approx(inside));  // untouched inside circle
}

TEST_CASE("vehicle: engine torque curve interpolates and clamps to endpoints",
          "[vehicle][drivetrain]") {
    const EngineCurvePoint c[] = {{1000.0f, 150.0f}, {3000.0f, 250.0f}, {6000.0f, 200.0f}};
    REQUIRE(engine_torque_at(c, 3, 500.0f) == Approx(150.0f));    // below first
    REQUIRE(engine_torque_at(c, 3, 7000.0f) == Approx(200.0f));   // above last
    REQUIRE(engine_torque_at(c, 3, 2000.0f) == Approx(200.0f));   // midpoint
    REQUIRE(engine_torque_at(nullptr, 0, 1000.0f) == Approx(0.0f));
}

TEST_CASE("vehicle: aero drag and downforce scale with v squared",
          "[vehicle][aero]") {
    const float cd = 0.30f;
    const float area = 2.20f;
    const float d10 = aero_drag_force(cd, area, 10.0f);
    REQUIRE(d10 == Approx(0.5f * 1.204f * cd * area * 100.0f));
    REQUIRE(aero_drag_force(cd, area, 20.0f) == Approx(4.0f * d10).margin(1e-2f));
    REQUIRE(aero_downforce(0.50f, 0.0f) == Approx(0.0f));
    REQUIRE(aero_downforce(0.50f, 30.0f) > 0.0f);
}

TEST_CASE("vehicle: suspension never pulls and stiffens past the bump-stop",
          "[vehicle][suspension]") {
    const float k = 22000.0f;
    const float c = 2800.0f;
    REQUIRE(suspension_force(k, c, 0.0f, 0.0f, 0.20f) == Approx(0.0f));
    REQUIRE(suspension_force(k, c, -0.05f, 0.0f, 0.20f) == Approx(0.0f));  // droop
    REQUIRE(suspension_force(k, c, 0.10f, 0.0f, 0.20f) == Approx(k * 0.10f));
    REQUIRE(suspension_force(k, c, 0.25f, 0.0f, 0.20f) > k * 0.25f);  // bump-stop adds
}

TEST_CASE("vehicle: quaternion helpers rotate and integrate correctly",
          "[vehicle][math]") {
    REQUIRE(quat_rotate(Quat{0, 0, 0, 1}, {0, 0, 1}).z == Approx(1.0f));
    const float h = 0.70710678f;  // 90 deg about +Y maps +Z -> +X
    const Vec3 r = quat_rotate(Quat{0, h, 0, h}, {0, 0, 1});
    REQUIRE(r.x == Approx(1.0f).margin(1e-4f));
    REQUIRE(r.z == Approx(0.0f).margin(1e-4f));
    const Quat q = quat_integrate(Quat{0, 0, 0, 1}, {0, 0, 0}, 0.1f);
    REQUIRE(q.w == Approx(1.0f));
}

// ─── Tier 2: integration smoke through the public API ─────────────────────────

TEST_CASE("vehicle: settles on its suspension without sinking through ground",
          "[vehicle][suspension][smoke]") {
    CarRig rig;
    Vehicle* v = create_vehicle(nullptr, rig.desc);
    REQUIRE(v != nullptr);
    settle(v, 300);

    float p[3], q[4];
    vehicle_get_transform(v, p, q);
    REQUIRE(p[1] > 0.40f);                       // resting above the ground plane
    REQUIRE(p[1] < 1.20f);                       // didn't launch off the springs
    REQUIRE(vehicle_get_speed_mps(v) < 0.20f);   // at rest
    REQUIRE(std::fabs(q[3]) > 0.99f);            // stayed upright (w ~ 1)
    destroy_vehicle(v);
}

TEST_CASE("vehicle: accelerates forward under throttle in first gear",
          "[vehicle][drivetrain][smoke]") {
    CarRig rig;
    Vehicle* v = create_vehicle(nullptr, rig.desc);
    settle(v);

    float p0[3], q[4];
    vehicle_get_transform(v, p0, q);
    const float speed0 = vehicle_get_speed_mps(v);

    VehicleInput in{};
    in.throttle = 1.0f;
    in.gear_request = +1;  // neutral -> first
    vehicle_tick(v, in, kDt);
    in.gear_request = 0;
    for (int i = 0; i < 360; ++i) vehicle_tick(v, in, kDt);  // 3 s

    float p1[3];
    vehicle_get_transform(v, p1, q);
    REQUIRE(vehicle_get_speed_mps(v) > speed0 + 5.0f);
    REQUIRE(p1[2] > p0[2] + 2.0f);  // travelled forward (+Z)
    destroy_vehicle(v);
}

TEST_CASE("vehicle: upshifting beats the redline-limited first gear",
          "[vehicle][drivetrain][smoke]") {
    // First gear tops out at the rev limiter (~20 m/s for this rig). Climbing
    // through the box on a speed schedule keeps the engine in its band and
    // reaches a speed first gear alone cannot.
    auto top_speed = [](bool upshift) {
        CarRig rig;
        Vehicle* v = create_vehicle(nullptr, rig.desc);
        settle(v);
        VehicleInput in{};
        in.throttle = 1.0f;
        in.gear_request = +1;  // neutral -> first
        vehicle_tick(v, in, kDt);
        // Shift up near the top of each gear (just under each redline cap).
        const float shift_at[] = {18.0f, 28.0f, 39.0f, 52.0f};
        int gear = 1;
        for (int i = 0; i < 120 * 12; ++i) {  // 12 s
            in.gear_request = 0;
            if (upshift && gear <= 4 && vehicle_get_speed_mps(v) > shift_at[gear - 1]) {
                in.gear_request = +1;
                ++gear;
            }
            vehicle_tick(v, in, kDt);
        }
        const float s = vehicle_get_speed_mps(v);
        destroy_vehicle(v);
        return s;
    };
    const float first_only = top_speed(false);
    const float through_gears = top_speed(true);
    REQUIRE(first_only < 22.0f);                 // capped by the rev limiter
    REQUIRE(through_gears > 26.0f);              // only reachable in higher gears
    REQUIRE(through_gears > first_only + 5.0f);  // gearbox unlocks more speed
}

TEST_CASE("vehicle: braking sheds speed", "[vehicle][smoke]") {
    CarRig rig;
    Vehicle* v = create_vehicle(nullptr, rig.desc);
    settle(v);

    VehicleInput go{};
    go.throttle = 1.0f;
    go.gear_request = +1;
    vehicle_tick(v, go, kDt);
    go.gear_request = 0;
    for (int i = 0; i < 240; ++i) vehicle_tick(v, go, kDt);  // 2 s accel
    const float fast = vehicle_get_speed_mps(v);
    REQUIRE(fast > 5.0f);

    VehicleInput brake{};
    brake.brake = 1.0f;
    for (int i = 0; i < 240; ++i) vehicle_tick(v, brake, kDt);  // 2 s brake
    REQUIRE(vehicle_get_speed_mps(v) < fast - 3.0f);
    destroy_vehicle(v);
}

TEST_CASE("vehicle: reverse gear drives backward", "[vehicle][drivetrain]") {
    CarRig rig;
    Vehicle* v = create_vehicle(nullptr, rig.desc);
    settle(v);

    float p0[3], q[4];
    vehicle_get_transform(v, p0, q);

    VehicleInput rev{};
    rev.throttle = 0.8f;
    rev.gear_request = -1;  // neutral -> reverse
    vehicle_tick(v, rev, kDt);
    rev.gear_request = 0;
    for (int i = 0; i < 300; ++i) vehicle_tick(v, rev, kDt);  // 2.5 s

    float p1[3];
    vehicle_get_transform(v, p1, q);
    REQUIRE(p1[2] < p0[2] - 1.0f);  // travelled backward (-Z)
    destroy_vehicle(v);
}

TEST_CASE("vehicle: low grip breaks traction under throttle",
          "[vehicle][tire][smoke]") {
    // Identical full-throttle launch on dry tarmac vs ice. On ice the driven
    // tires exceed the friction circle and spin (traction broken), so the car
    // barely accelerates next to the gripping surface.
    auto launch_speed = [](float mu) {
        CarRig rig;
        for (WheelDesc& w : rig.wheels) w.tire_friction_mu = mu;
        Vehicle* v = create_vehicle(nullptr, rig.desc);
        settle(v);
        VehicleInput in{};
        in.throttle = 1.0f;
        in.gear_request = +1;  // neutral -> first
        vehicle_tick(v, in, kDt);
        in.gear_request = 0;
        for (int i = 0; i < 240; ++i) vehicle_tick(v, in, kDt);  // 2 s
        const float s = vehicle_get_speed_mps(v);
        destroy_vehicle(v);
        return s;
    };
    const float dry = launch_speed(1.05f);
    const float ice = launch_speed(0.20f);
    REQUIRE(dry > 6.0f);        // grip puts the power down
    REQUIRE(ice < dry - 3.0f);  // ice spins the tires -> much less go
}

TEST_CASE("vehicle: identical inputs produce bit-identical motion "
          "(determinism precondition)",
          "[vehicle][determinism]") {
    auto run = [](float* out, int n) {
        CarRig rig;
        Vehicle* v = create_vehicle(nullptr, rig.desc);
        VehicleInput in{};
        in.throttle = 0.80f;
        in.steer = 0.30f;
        in.gear_request = +1;
        for (int i = 0; i < n; ++i) {
            vehicle_tick(v, in, kDt);
            in.gear_request = 0;
            float p[3], q[4];
            vehicle_get_transform(v, p, q);
            out[i * 4 + 0] = p[0];
            out[i * 4 + 1] = p[1];
            out[i * 4 + 2] = p[2];
            out[i * 4 + 3] = vehicle_get_speed_mps(v);
        }
        destroy_vehicle(v);
    };
    constexpr int n = 200;
    float a[n * 4]{};
    float b[n * 4]{};
    run(a, n);
    run(b, n);
    for (int i = 0; i < n * 4; ++i) REQUIRE(a[i] == b[i]);  // bit-exact
}
