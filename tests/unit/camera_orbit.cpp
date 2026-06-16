// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_orbit.cpp
//
// Lane 16 — third-person orbit / chase camera. Validates the contract in
// engine/camera/Orbit.h:
//   - At yaw 0 / pitch 0 the eye sits at target + (0,0,+distance) (behind along
//     +Z, since forward is -Z) and forward is (0,0,-1) toward the target.
//   - |eye - target| == distance_m for any yaw / pitch (the eye lives on the
//     orbit sphere).
//   - A positive pitch (look up) raises the eye above the target while still
//     pointing the look back at it.
//   - forward == normalize(target - eye) — the camera looks at the orbit centre.
//   - orbit_rotate wraps yaw into [-180,180] and clamps pitch to [-89,89].
//   - orbit_zoom clamps the radius into [min,max].
//   - The result yaw/pitch equal the state's.
//   - Two identical orbit_eye calls are bit-identical (same-platform
//     determinism for the cosmetic view lane).

#include "camera/Orbit.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::camera;

namespace {

f32 dist(const f32 a[3], const f32 b[3]) noexcept {
    const f32 dx = a[0] - b[0];
    const f32 dy = a[1] - b[1];
    const f32 dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

TEST_CASE("orbit: yaw0 pitch0 places eye behind along plusZ looking down minusZ",
          "[camera]") {
    OrbitState s{};
    s.target[0] = 1.0f;
    s.target[1] = 2.0f;
    s.target[2] = 3.0f;
    s.yaw_deg    = 0.0f;
    s.pitch_deg  = 0.0f;
    s.distance_m = 5.0f;

    const OrbitResult r = orbit_eye(s);

    // forward is the canonical -Z look.
    REQUIRE(r.forward[0] == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(r.forward[1] == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(r.forward[2] == Catch::Approx(-1.0f));

    // eye = target - forward*distance = target + (0,0,+distance).
    REQUIRE(r.eye[0] == Catch::Approx(1.0f));
    REQUIRE(r.eye[1] == Catch::Approx(2.0f));
    REQUIRE(r.eye[2] == Catch::Approx(3.0f + 5.0f));
}

TEST_CASE("orbit: eye is always distance from the target on the sphere",
          "[camera]") {
    OrbitState s{};
    s.target[0] = -2.0f;
    s.target[1] =  4.0f;
    s.target[2] =  7.0f;
    s.distance_m = 6.5f;

    // Sweep a range of yaw and pitch; the radius invariant must hold at each.
    const f32 yaws[]   = {-180.0f, -90.0f, -33.0f, 0.0f, 45.0f, 120.0f, 180.0f};
    const f32 pitches[] = {-89.0f, -45.0f, -10.0f, 0.0f, 10.0f, 60.0f, 89.0f};

    for (f32 yw : yaws) {
        for (f32 pt : pitches) {
            s.yaw_deg   = yw;
            s.pitch_deg = pt;
            const OrbitResult r = orbit_eye(s);
            REQUIRE(dist(r.eye, s.target) == Catch::Approx(s.distance_m));
        }
    }
}

TEST_CASE("orbit: positive pitch raises the eye above the target", "[camera]") {
    OrbitState s{};
    s.target[0] = 0.0f;
    s.target[1] = 10.0f;
    s.target[2] = 0.0f;
    s.distance_m = 4.0f;
    s.yaw_deg    = 0.0f;
    s.pitch_deg  = 30.0f;  // look up

    const OrbitResult r = orbit_eye(s);

    // Looking up tilts forward toward +Y; eye = target - forward*d subtracts a
    // positive Y, so the eye sits ABOVE the target.
    REQUIRE(r.eye[1] > s.target[1]);

    // And it still looks back at the target.
    f32 to_target[3] = {s.target[0] - r.eye[0],
                        s.target[1] - r.eye[1],
                        s.target[2] - r.eye[2]};
    const f32 len = std::sqrt(to_target[0] * to_target[0] +
                              to_target[1] * to_target[1] +
                              to_target[2] * to_target[2]);
    to_target[0] /= len;
    to_target[1] /= len;
    to_target[2] /= len;
    REQUIRE(to_target[0] == Catch::Approx(r.forward[0]).margin(1e-5f));
    REQUIRE(to_target[1] == Catch::Approx(r.forward[1]).margin(1e-5f));
    REQUIRE(to_target[2] == Catch::Approx(r.forward[2]).margin(1e-5f));
}

TEST_CASE("orbit: forward points from eye toward the target", "[camera]") {
    OrbitState s{};
    s.target[0] = 3.0f;
    s.target[1] = -1.0f;
    s.target[2] = 8.0f;
    s.distance_m = 9.0f;
    s.yaw_deg    = 57.0f;
    s.pitch_deg  = -22.0f;

    const OrbitResult r = orbit_eye(s);

    // normalize(target - eye) must equal the reported unit forward.
    f32 to_target[3] = {s.target[0] - r.eye[0],
                        s.target[1] - r.eye[1],
                        s.target[2] - r.eye[2]};
    const f32 len = std::sqrt(to_target[0] * to_target[0] +
                              to_target[1] * to_target[1] +
                              to_target[2] * to_target[2]);
    REQUIRE(len == Catch::Approx(s.distance_m));  // also re-confirms the radius

    REQUIRE(to_target[0] / len == Catch::Approx(r.forward[0]).margin(1e-5f));
    REQUIRE(to_target[1] / len == Catch::Approx(r.forward[1]).margin(1e-5f));
    REQUIRE(to_target[2] / len == Catch::Approx(r.forward[2]).margin(1e-5f));

    // forward is unit length.
    const f32 flen = std::sqrt(r.forward[0] * r.forward[0] +
                               r.forward[1] * r.forward[1] +
                               r.forward[2] * r.forward[2]);
    REQUIRE(flen == Catch::Approx(1.0f));
}

TEST_CASE("orbit: result yaw and pitch equal the state", "[camera]") {
    OrbitState s{};
    s.yaw_deg   = 123.5f;
    s.pitch_deg = -41.25f;
    const OrbitResult r = orbit_eye(s);
    REQUIRE(r.yaw_deg   == s.yaw_deg);
    REQUIRE(r.pitch_deg == s.pitch_deg);
}

TEST_CASE("orbit: rotate wraps yaw into the plusMinus180 band", "[camera]") {
    OrbitState s{};
    s.yaw_deg = 170.0f;

    // +30 crosses +180 and must wrap to -160.
    orbit_rotate(s, 30.0f, 0.0f);
    REQUIRE(s.yaw_deg == Catch::Approx(-160.0f));
    REQUIRE(s.yaw_deg >= -180.0f);
    REQUIRE(s.yaw_deg <= 180.0f);

    // A large negative spin also lands back in range.
    s.yaw_deg = -170.0f;
    orbit_rotate(s, -50.0f, 0.0f);
    REQUIRE(s.yaw_deg >= -180.0f);
    REQUIRE(s.yaw_deg <= 180.0f);
    REQUIRE(s.yaw_deg == Catch::Approx(140.0f));

    // A huge multi-turn delta still wraps in O(1) to within the band.
    s.yaw_deg = 0.0f;
    orbit_rotate(s, 3600.0f + 45.0f, 0.0f);
    REQUIRE(s.yaw_deg >= -180.0f);
    REQUIRE(s.yaw_deg <= 180.0f);
    REQUIRE(s.yaw_deg == Catch::Approx(45.0f));
}

TEST_CASE("orbit: rotate clamps pitch to plusMinus89", "[camera]") {
    OrbitState s{};
    s.pitch_deg = 80.0f;

    // Push past the top — clamps at +89.
    orbit_rotate(s, 0.0f, 50.0f);
    REQUIRE(s.pitch_deg == Catch::Approx(89.0f));

    // Push past the bottom — clamps at -89.
    orbit_rotate(s, 0.0f, -1000.0f);
    REQUIRE(s.pitch_deg == Catch::Approx(-89.0f));

    // A normal step in range accumulates exactly.
    s.pitch_deg = 0.0f;
    orbit_rotate(s, 0.0f, 12.5f);
    REQUIRE(s.pitch_deg == Catch::Approx(12.5f));
}

TEST_CASE("orbit: zoom clamps the radius into the range", "[camera]") {
    OrbitState s{};
    s.distance_m = 5.0f;

    // Zoom in past the floor — clamps at min.
    orbit_zoom(s, -100.0f, 2.0f, 10.0f);
    REQUIRE(s.distance_m == Catch::Approx(2.0f));

    // Zoom out past the ceiling — clamps at max.
    orbit_zoom(s, 100.0f, 2.0f, 10.0f);
    REQUIRE(s.distance_m == Catch::Approx(10.0f));

    // A small step inside the band applies exactly.
    s.distance_m = 5.0f;
    orbit_zoom(s, 1.5f, 2.0f, 10.0f);
    REQUIRE(s.distance_m == Catch::Approx(6.5f));

    // A swapped (degenerate) range is normalised so the clamp still bands it.
    s.distance_m = 100.0f;
    orbit_zoom(s, 0.0f, 10.0f, 2.0f);  // min > max on purpose
    REQUIRE(s.distance_m == Catch::Approx(10.0f));
}

TEST_CASE("orbit: identical inputs produce bit-identical eyes (determinism)",
          "[camera]") {
    OrbitState s{};
    s.target[0] = 1.5f;
    s.target[1] = -3.0f;
    s.target[2] = 2.25f;
    s.yaw_deg    = 73.0f;
    s.pitch_deg  = 18.0f;
    s.distance_m = 7.5f;

    const OrbitResult a = orbit_eye(s);
    const OrbitResult b = orbit_eye(s);

    // Bit-identical (== on the raw f32s, not Approx) — same-platform guarantee.
    REQUIRE(a.eye[0] == b.eye[0]);
    REQUIRE(a.eye[1] == b.eye[1]);
    REQUIRE(a.eye[2] == b.eye[2]);
    REQUIRE(a.forward[0] == b.forward[0]);
    REQUIRE(a.forward[1] == b.forward[1]);
    REQUIRE(a.forward[2] == b.forward[2]);
    REQUIRE(a.yaw_deg   == b.yaw_deg);
    REQUIRE(a.pitch_deg == b.pitch_deg);
}
