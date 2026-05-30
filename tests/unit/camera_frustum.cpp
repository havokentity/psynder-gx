// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_frustum.cpp
//
// Lane 16 — view-frustum extraction + AABB / sphere culling. Validates the
// contract documented in engine/camera/Frustum.h:
//   - frustum_from_view_proj extracts six normalized planes (Gribb-Hartmann)
//     from this engine's column-major view-projection matrix.
//   - A point / small box / small sphere clearly IN FRONT of and centered in
//     the view is reported visible (Inside).
//   - A box BEHIND the camera is culled (Outside / not visible).
//   - A box far to the SIDE beyond the FOV is culled.
//   - A big box straddling the near plane classifies as Intersect.
//   - sphere_in_frustum agrees with aabb_in_frustum for a tiny box vs a tiny
//     sphere at the same point.
//   - Every extracted plane normal is unit length.
//   - The extraction + culling are deterministic: the same matrix yields
//     bit-identical planes and identical cull results.
//
// Camera model: at the world origin looking down -Z (yaw 0, pitch 0), +Y up,
// fov_y = 60 deg, aspect = 16/9, near = 0.1 m, far = 100 m. With -Z forward,
// "in front" means negative z; "behind" means positive z.

#include "camera/Frustum.h"
#include "camera/Camera.h"
#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::camera;

namespace {

constexpr f32 kAspect = 16.0f / 9.0f;

// CameraState at the origin, looking down -Z, with the chosen params.
CameraState make_cam() noexcept {
    CameraState cam{};
    cam.position[0] = 0.0f;
    cam.position[1] = 0.0f;
    cam.position[2] = 0.0f;
    cam.yaw_deg     = 0.0f;   // forward = (0, 0, -1)
    cam.pitch_deg   = 0.0f;
    cam.fov_y_deg   = 60.0f;
    cam.near_m      = 0.1f;
    cam.far_m       = 100.0f;
    return cam;
}

// A tiny axis-aligned box centered at p (half-extent 0.05 m).
void tiny_box(math::Vec3 p, math::Vec3& mn, math::Vec3& mx) noexcept {
    constexpr f32 h = 0.05f;
    mn = { p.x - h, p.y - h, p.z - h };
    mx = { p.x + h, p.y + h, p.z + h };
}

}  // namespace

TEST_CASE("frustum: extraction yields six unit-length plane normals",
          "[camera]") {
    const CameraState cam = make_cam();
    const math::Mat4 vp = view_proj_matrix(cam, kAspect);
    const Frustum fr = frustum_from_view_proj(vp);

    for (u32 i = 0; i < Frustum::kCount; ++i) {
        const Plane& p = fr.planes[i];
        const f32 len = std::sqrt(p.nx * p.nx + p.ny * p.ny + p.nz * p.nz);
        REQUIRE(len == Catch::Approx(1.0f).margin(1e-5f));
    }
}

TEST_CASE("frustum: a point and small volume centered in front is inside",
          "[camera]") {
    const CameraState cam = make_cam();
    const Frustum fr = frustum_from_view_proj(view_proj_matrix(cam, kAspect));

    // 10 m straight ahead (down -Z), dead center.
    const math::Vec3 ahead{ 0.0f, 0.0f, -10.0f };

    math::Vec3 mn, mx;
    tiny_box(ahead, mn, mx);

    REQUIRE(aabb_in_frustum(fr, mn, mx));
    REQUIRE(classify_aabb(fr, mn, mx) == Cull::Inside);
    REQUIRE(sphere_in_frustum(fr, ahead, 0.05f));
}

TEST_CASE("frustum: a volume behind the camera is culled", "[camera]") {
    const CameraState cam = make_cam();
    const Frustum fr = frustum_from_view_proj(view_proj_matrix(cam, kAspect));

    // 10 m BEHIND the camera (positive z, since forward is -Z).
    const math::Vec3 behind{ 0.0f, 0.0f, 10.0f };

    math::Vec3 mn, mx;
    tiny_box(behind, mn, mx);

    REQUIRE_FALSE(aabb_in_frustum(fr, mn, mx));
    REQUIRE(classify_aabb(fr, mn, mx) == Cull::Outside);
    REQUIRE_FALSE(sphere_in_frustum(fr, behind, 0.05f));
}

TEST_CASE("frustum: a volume far to the side beyond the fov is culled",
          "[camera]") {
    const CameraState cam = make_cam();
    const Frustum fr = frustum_from_view_proj(view_proj_matrix(cam, kAspect));

    // 10 m ahead but 100 m to the right — way outside the 60-deg vertical /
    // ~92-deg horizontal cone at that depth.
    const math::Vec3 side{ 100.0f, 0.0f, -10.0f };

    math::Vec3 mn, mx;
    tiny_box(side, mn, mx);

    REQUIRE_FALSE(aabb_in_frustum(fr, mn, mx));
    REQUIRE(classify_aabb(fr, mn, mx) == Cull::Outside);
    REQUIRE_FALSE(sphere_in_frustum(fr, side, 0.05f));

    // Likewise far ABOVE the view (beyond the vertical fov).
    const math::Vec3 above{ 0.0f, 100.0f, -10.0f };
    math::Vec3 amn, amx;
    tiny_box(above, amn, amx);
    REQUIRE_FALSE(aabb_in_frustum(fr, amn, amx));
}

TEST_CASE("frustum: a big box straddling the near plane intersects",
          "[camera]") {
    const CameraState cam = make_cam();
    const Frustum fr = frustum_from_view_proj(view_proj_matrix(cam, kAspect));

    // A large box that spans from behind the camera to well in front, centered
    // on the view axis. It pokes through the near plane (z = -0.1) so the
    // farthest corner is inside but the nearest corner is outside => Intersect.
    const math::Vec3 mn{ -1.0f, -1.0f, -5.0f };
    const math::Vec3 mx{  1.0f,  1.0f,  5.0f };

    // It is at least possibly-visible (the +z, in-front half is inside).
    REQUIRE(aabb_in_frustum(fr, mn, mx));
    REQUIRE(classify_aabb(fr, mn, mx) == Cull::Intersect);
}

TEST_CASE("frustum: sphere test agrees with aabb test for a tiny coincident "
          "volume",
          "[camera]") {
    const CameraState cam = make_cam();
    const Frustum fr = frustum_from_view_proj(view_proj_matrix(cam, kAspect));

    // Sample a handful of positions; a tiny box and a tiny sphere at the same
    // center must reach the same visible / culled verdict.
    const math::Vec3 samples[] = {
        { 0.0f,  0.0f,  -10.0f },   // ahead, visible
        { 0.0f,  0.0f,   10.0f },   // behind, culled
        { 100.0f, 0.0f,  -10.0f },  // far side, culled
        { 0.0f,  100.0f, -10.0f },  // far above, culled
        { 2.0f,  1.0f,  -50.0f },   // off-center but inside, visible
    };

    for (const math::Vec3& s : samples) {
        math::Vec3 mn, mx;
        tiny_box(s, mn, mx);
        const bool box_vis    = aabb_in_frustum(fr, mn, mx);
        const bool sphere_vis = sphere_in_frustum(fr, s, 0.05f);
        REQUIRE(box_vis == sphere_vis);
    }
}

TEST_CASE("frustum: extraction and culling are deterministic for the same "
          "matrix",
          "[camera]") {
    const CameraState cam = make_cam();
    const math::Mat4 vp = view_proj_matrix(cam, kAspect);

    const Frustum a = frustum_from_view_proj(vp);
    const Frustum b = frustum_from_view_proj(vp);

    // Bit-identical planes (== on the raw f32s, not Approx).
    for (u32 i = 0; i < Frustum::kCount; ++i) {
        REQUIRE(a.planes[i].nx == b.planes[i].nx);
        REQUIRE(a.planes[i].ny == b.planes[i].ny);
        REQUIRE(a.planes[i].nz == b.planes[i].nz);
        REQUIRE(a.planes[i].d  == b.planes[i].d);
    }

    // Identical cull verdicts across a few probes.
    const math::Vec3 probes[] = {
        { 0.0f, 0.0f, -10.0f },
        { 0.0f, 0.0f,  10.0f },
        { 50.0f, 0.0f, -10.0f },
    };
    for (const math::Vec3& p : probes) {
        math::Vec3 mn, mx;
        tiny_box(p, mn, mx);
        REQUIRE(aabb_in_frustum(a, mn, mx) == aabb_in_frustum(b, mn, mx));
        REQUIRE(classify_aabb(a, mn, mx)   == classify_aabb(b, mn, mx));
        REQUIRE(sphere_in_frustum(a, p, 0.05f) == sphere_in_frustum(b, p, 0.05f));
    }
}
