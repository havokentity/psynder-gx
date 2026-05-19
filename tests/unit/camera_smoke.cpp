// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_smoke.cpp
//
// Lane 16 — Camera + Input glue smoke tests. Validates the FPS-controller
// contract documented in engine/camera/Camera.h:
//   - Default-constructed state produces finite matrices.
//   - Yaw=+90° looks toward +X (project convention).
//   - Pitch clamps to ±89° even with grossly out-of-range input.
//   - Translation: forward_axis=1, dt=1, speed=v → exactly v metres
//     along the forward vector.
//   - View+proj round-trip: a world-space point at z = -1 m projects to
//     normalized device coordinates whose unprojected x/y matches the
//     original within 1e-3 m at z = -1.
//   - Non-square aspects produce finite, non-degenerate proj matrices.

#include "camera/Camera.h"
#include "math/Math.h"
#include "math/MathExt.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::camera;

namespace {

bool approx_eq(f32 a, f32 b, f32 tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

bool all_finite(const math::Mat4& m) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m.m[i])) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("camera: default state produces a finite view matrix",
          "[camera][smoke]") {
    CameraState cam{};
    math::Mat4 v = view_matrix(cam);
    REQUIRE(all_finite(v));

    // The default position is the world origin; look_at_rh of an
    // origin-located camera looking down -Z should NOT be the identity
    // (it has a sign flip on the z column), but it MUST be finite.
    // Spot-check: m[11] is reserved for perspective; in a pure view
    // matrix m[15] == 1 and m[11] == 0.
    REQUIRE(approx_eq(v.m[15], 1.0f));
    REQUIRE(approx_eq(v.m[11], 0.0f));

    // Default proj at aspect=16:9 is also finite + non-degenerate.
    math::Mat4 p = proj_matrix(cam, 16.0f / 9.0f);
    REQUIRE(all_finite(p));
    REQUIRE(approx_eq(p.m[11], -1.0f));   // perspective_rh's -1 entry
}

TEST_CASE("camera: yaw=+90 looks toward +X",
          "[camera][smoke]") {
    // Per the convention header in Camera.h:
    //   yaw =   0° → forward = (0, 0, -1)
    //   yaw = +90° → forward = (+1, 0, 0)
    CameraState cam{};

    // Yaw=0 baseline.
    math::Vec3 fwd0 = forward_vector(cam);
    REQUIRE(approx_eq(fwd0.x,  0.0f));
    REQUIRE(approx_eq(fwd0.y,  0.0f));
    REQUIRE(approx_eq(fwd0.z, -1.0f));

    // Yaw=+90°: forward should rotate from -Z to +X.
    cam.yaw_deg = 90.0f;
    math::Vec3 fwd90 = forward_vector(cam);
    REQUIRE(approx_eq(fwd90.x, 1.0f));
    REQUIRE(approx_eq(fwd90.y, 0.0f));
    REQUIRE(approx_eq(fwd90.z, 0.0f));

    // And the right vector at yaw=+90° should be +Z (right is 90° CW of
    // forward when viewed from world-up).
    math::Vec3 right90 = right_vector(cam);
    REQUIRE(approx_eq(right90.x, 0.0f));
    REQUIRE(approx_eq(right90.y, 0.0f));
    REQUIRE(approx_eq(right90.z, 1.0f));

    // up_local at yaw=+90°, pitch=0 is still world +Y.
    math::Vec3 up90 = up_vector(cam);
    REQUIRE(approx_eq(up90.x, 0.0f));
    REQUIRE(approx_eq(up90.y, 1.0f));
    REQUIRE(approx_eq(up90.z, 0.0f));
}

TEST_CASE("camera: pitch clamps to +/-89 degrees",
          "[camera][smoke]") {
    CameraState cam{};

    // Slam pitch waaaay above +89.
    {
        CameraInput in{};
        in.pitch_delta_deg = 200.0f;
        in.dt_sec          = 0.016f;
        update_camera(cam, in);
        REQUIRE(approx_eq(cam.pitch_deg, 89.0f));
    }

    // And waaaay below -89.
    {
        CameraInput in{};
        in.pitch_delta_deg = -400.0f;   // overshoots the +89→-89 swing too
        in.dt_sec          = 0.016f;
        update_camera(cam, in);
        REQUIRE(approx_eq(cam.pitch_deg, -89.0f));
    }

    // Verify the inverse-pitch flag also respects the clamp.
    {
        cam.pitch_deg      = 0.0f;
        CameraInput in{};
        in.pitch_delta_deg = 200.0f;
        in.invert_pitch    = true;
        in.dt_sec          = 0.016f;
        update_camera(cam, in);
        REQUIRE(approx_eq(cam.pitch_deg, -89.0f));
    }
}

TEST_CASE("camera: forward_axis=1, dt=1 advances exactly move_speed_m_s "
          "along forward",
          "[camera][smoke]") {
    // At yaw=0, pitch=0 the forward vector is exactly (0, 0, -1). Driving
    // forward_axis=+1 for one second at the default 5 m/s should land the
    // camera at (0, 0, -5).
    CameraState cam{};
    CameraInput in{};
    in.forward_axis   = 1.0f;
    in.dt_sec         = 1.0f;
    in.move_speed_m_s = 5.0f;

    update_camera(cam, in);

    REQUIRE(approx_eq(cam.position[0],  0.0f));
    REQUIRE(approx_eq(cam.position[1],  0.0f));
    REQUIRE(approx_eq(cam.position[2], -5.0f));

    // And running a second step at a different speed accumulates.
    in.move_speed_m_s = 2.0f;
    in.dt_sec         = 0.5f;
    update_camera(cam, in);
    REQUIRE(approx_eq(cam.position[2], -6.0f));   // -5 + (-1)
}

TEST_CASE("camera: view_proj round-trips a point at z=-1 m within 1e-3 m",
          "[camera][smoke]") {
    // Pick a non-default camera state to exercise the full pipeline.
    CameraState cam{};
    cam.position[0] = 0.0f;
    cam.position[1] = 0.0f;
    cam.position[2] = 0.0f;
    cam.yaw_deg     = 0.0f;
    cam.pitch_deg   = 0.0f;
    cam.fov_y_deg   = 60.0f;
    cam.near_m      = 0.05f;
    cam.far_m       = 1000.0f;

    const f32 aspect = 16.0f / 9.0f;

    // World point at z = -1 m, directly in front of the camera. Pick an
    // off-axis (x, y) so the projection isn't trivially at the screen
    // centre — that way the round-trip actually tests the perspective
    // math, not just the divide-by-w.
    const f32 world_x = 0.25f;
    const f32 world_y = 0.10f;
    const f32 world_z = -1.0f;

    math::Mat4 vp = view_proj_matrix(cam, aspect);

    // Forward: world → clip → NDC (divide by w).
    math::Vec4 clip = math::mul(vp, math::Vec4{world_x, world_y, world_z, 1.0f});
    REQUIRE(std::isfinite(clip.w));
    REQUIRE(std::fabs(clip.w) > 1e-6f);

    math::Vec4 ndc{ clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, 1.0f };

    // Reverse: NDC → clip (with w from forward) → world via inverse(vp).
    math::Vec4 clip_back{ ndc.x * clip.w, ndc.y * clip.w, ndc.z * clip.w,
                          clip.w };

    math::Mat4 inv_vp = math::inverse(vp);
    math::Vec4 back   = math::mul(inv_vp, clip_back);

    // Homogeneous divide.
    REQUIRE(std::isfinite(back.w));
    REQUIRE(std::fabs(back.w) > 1e-6f);
    const f32 rx = back.x / back.w;
    const f32 ry = back.y / back.w;
    const f32 rz = back.z / back.w;

    REQUIRE(approx_eq(rx, world_x, 1e-3f));
    REQUIRE(approx_eq(ry, world_y, 1e-3f));
    REQUIRE(approx_eq(rz, world_z, 1e-3f));
}

TEST_CASE("camera: non-square aspects still produce a non-degenerate proj "
          "matrix",
          "[camera][smoke]") {
    CameraState cam{};   // defaults

    for (f32 aspect : { 0.5f, 1.0f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f }) {
        math::Mat4 p = proj_matrix(cam, aspect);
        REQUIRE(all_finite(p));

        // The conventional perspective_rh entries: m[0]=f/aspect, m[5]=f.
        // Both non-zero, finite, and consistent in sign.
        REQUIRE(p.m[0]  > 0.0f);
        REQUIRE(p.m[5]  > 0.0f);
        REQUIRE(approx_eq(p.m[11], -1.0f));

        // Determinant non-trivial — a degenerate / rank-deficient proj
        // would have det = 0.
        f32 d = math::determinant(p);
        REQUIRE(std::fabs(d) > 1e-6f);
    }

    // Degenerate aspect (0 / negative / NaN) is silently clamped to 1.0
    // inside proj_matrix so callers can survive a window-resize race
    // where the swapchain comes back as (0, 0) for one frame.
    math::Mat4 p0 = proj_matrix(cam, 0.0f);
    REQUIRE(all_finite(p0));
    math::Mat4 p_neg = proj_matrix(cam, -2.0f);
    REQUIRE(all_finite(p_neg));
    math::Mat4 p_nan = proj_matrix(cam, std::nanf(""));
    REQUIRE(all_finite(p_nan));
}

TEST_CASE("camera: strafe moves along the local right axis",
          "[camera][smoke]") {
    // Yaw=0 → right vector = (+1, 0, 0). strafe_axis=+1, dt=1, speed=3
    // → camera moves to (+3, 0, 0).
    CameraState cam{};
    CameraInput in{};
    in.strafe_axis    = 1.0f;
    in.dt_sec         = 1.0f;
    in.move_speed_m_s = 3.0f;
    update_camera(cam, in);
    REQUIRE(approx_eq(cam.position[0],  3.0f));
    REQUIRE(approx_eq(cam.position[1],  0.0f));
    REQUIRE(approx_eq(cam.position[2],  0.0f));

    // After yaw=+90° (looking +X), strafing right should advance +Z.
    cam.position[0] = 0.0f;
    cam.yaw_deg     = 90.0f;
    CameraInput in2{};
    in2.strafe_axis    = 1.0f;
    in2.dt_sec         = 1.0f;
    in2.move_speed_m_s = 4.0f;
    update_camera(cam, in2);
    REQUIRE(approx_eq(cam.position[0], 0.0f));
    REQUIRE(approx_eq(cam.position[2], 4.0f));
}

TEST_CASE("camera: up_axis moves along world +Y regardless of pitch",
          "[camera][smoke]") {
    // up_axis is wired to world_up so jet-pack thrust isn't tilted by
    // where the player is looking. Verify across two pitches.
    for (f32 pitch : { -45.0f, 0.0f, 45.0f }) {
        CameraState cam{};
        cam.pitch_deg = pitch;
        CameraInput in{};
        in.up_axis        = 1.0f;
        in.dt_sec         = 1.0f;
        in.move_speed_m_s = 2.0f;
        update_camera(cam, in);
        REQUIRE(approx_eq(cam.position[0], 0.0f));
        REQUIRE(approx_eq(cam.position[1], 2.0f));
        REQUIRE(approx_eq(cam.position[2], 0.0f));
    }
}

TEST_CASE("camera: CameraState layout is the documented POD shape",
          "[camera][smoke]") {
    // Sanity: the struct stays trivially-copyable so future renderer code
    // can memcpy it into a GPU buffer.
    REQUIRE(std::is_trivially_copyable_v<CameraState>);
    REQUIRE(std::is_trivially_copyable_v<CameraInput>);
    REQUIRE(sizeof(CameraState) == 32);   // see Camera.h §POD layout note
}
