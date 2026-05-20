// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Camera.cpp
//
// Lane 16 — Camera + Input glue. See Camera.h for the public contract and
// the coordinate / yaw / pitch conventions.
//
// Determinism: this TU is built with `-fno-fast-math -ffp-contract=off`
// (Clang/GCC) or `/fp:strict` (MSVC). See engine/camera/CMakeLists.txt.

#include "Camera.h"

#include "math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace psynder::camera {
namespace {

// Local mini-helpers. Intentionally NOT added to engine/math/ — the brief
// requires lane 16 to stay self-contained and not expand the math surface.

inline f32 deg_to_rad(f32 deg) noexcept {
    return deg * math::kDegToRad;
}

// Clamp wrapper that keeps NaN out of the camera state. If `v` is NaN, the
// `v < lo` and `v > hi` comparisons are both false on IEEE 754, so the
// fallback path returns `lo` — better than letting NaN leak into the view
// matrix.
inline f32 clamp_finite(f32 v, f32 lo, f32 hi) noexcept {
    if (!(v == v)) return lo;             // NaN check (NaN != NaN)
    if (v < lo)    return lo;
    if (v > hi)    return hi;
    return v;
}

// Wrap an angle into [-180, +180].  Constant-time floor-based wrap (no
// while-loop) — for very large finite inputs the earlier while-loop form
// took O(|v|/360) iterations, which a single mouse-X spike could stretch
// into a multi-millisecond stall (Copilot PR #13 review).  Floor-form is
// O(1) and uses the same -fno-fast-math / /fp:strict arithmetic as the
// rest of the TU, so it stays deterministic on a given platform/libm.
//
// NaN guard returns 0 — a NaN escaping into yaw would poison every
// downstream matrix; 0 is a defensible fallback (neutral heading) and
// matches the clamp_finite recovery strategy used elsewhere here.
inline f32 wrap_deg_180(f32 v) noexcept {
    if (!(v == v)) return 0.0f;            // NaN → neutral heading
    // Fast path: already in range.
    if (v >= -180.0f && v <= 180.0f) return v;
    return v - std::floor((v + 180.0f) / 360.0f) * 360.0f;
}

inline math::Vec3 to_vec3(const f32 p[3]) noexcept {
    return { p[0], p[1], p[2] };
}

// Returns the (forward, right, up_local) basis from yaw_deg and pitch_deg.
// All three are unit vectors. See Camera.h §Conventions for the convention.
struct Basis {
    math::Vec3 forward;
    math::Vec3 right;
    math::Vec3 up_local;
};

inline Basis basis_from_angles(f32 yaw_deg, f32 pitch_deg) noexcept {
    const f32 y = deg_to_rad(yaw_deg);
    const f32 p = deg_to_rad(pitch_deg);

    const f32 sy = std::sin(y);
    const f32 cy = std::cos(y);
    const f32 sp = std::sin(p);
    const f32 cp = std::cos(p);

    // forward = ( cos(pitch)*sin(yaw),  sin(pitch), -cos(pitch)*cos(yaw) )
    // yaw=0 → forward = (0, 0, -1).
    // yaw=+90, pitch=0 → forward = (+1, 0, 0).
    const math::Vec3 forward = { cp * sy, sp, -cp * cy };

    // right is the strafe vector — perpendicular to the world-Y up axis
    // AND to the forward vector projected onto the horizontal plane. Using
    // the horizontal-only forward keeps strafing flat even when looking up
    // or down (the classic FPS feel).
    //   horiz_forward = (sin(yaw), 0, -cos(yaw))
    //   right = cross(horiz_forward, world_up) = (-cos(yaw), 0, -sin(yaw))
    // Verify at yaw=0: right = (-1, 0, 0). That's WRONG (we want +X).
    // The sign-flip is because cross(forward, up) in RH gives left, not
    // right, with the OpenGL -Z forward convention. So negate:
    //   right = cross(up, horiz_forward) = (cos(yaw), 0, sin(yaw))? Let me
    // redo the math: cross((0,1,0), (sin(yaw),0,-cos(yaw))) = (1*-cos(yaw)
    // - 0*0, 0*sin(yaw) - 0*-cos(yaw), 0*0 - 1*sin(yaw)) = (-cos(yaw), 0,
    // -sin(yaw)). At yaw=0: (-1, 0, 0). Still flipped.
    // Empirical: in our look_at_rh `s = normalize(cross(f, up))` where f
    // is the look direction. Look_at expects forward=-Z, so f×up =
    // (0,0,-1)×(0,1,0) = (0*0 - -1*1, -1*0 - 0*0, 0*1 - 0*0) = (1,0,0).
    // So s = +X = right. Matches GL convention.
    // Apply same f×up here with our forward at yaw=0: (0,0,-1)×(0,1,0) =
    // (+1, 0, 0). Right!
    const math::Vec3 world_up{ 0.0f, 1.0f, 0.0f };
    const math::Vec3 right_raw = math::cross(forward, world_up);
    const math::Vec3 right     = math::normalize(right_raw);

    // up_local closes the orthonormal frame. cross(right, forward) gives
    // the up vector perpendicular to BOTH the look direction and the
    // strafe direction — i.e. the local "head up" axis, which is +Y when
    // pitch=0 and tilts forward/back when pitch ≠ 0.
    const math::Vec3 up_local = math::cross(right, forward);

    return { forward, right, up_local };
}

}  // namespace

// ─── update_camera ───────────────────────────────────────────────────────

void update_camera(CameraState& cam, const CameraInput& in) noexcept {
    // 0) Sanitise inputs against NaN / Inf BEFORE they touch cam.* —
    //    otherwise a single bad frame (timer glitch, uninitialised input,
    //    network desync) poisons the camera's position and every
    //    subsequent matrix.  Non-finite values become 0; negatives are
    //    kept where they're semantically valid (axes ±, deltas ±) but
    //    pruned to 0 where they're not (dt_sec ≥ 0, speed ≥ 0).  No
    //    upper-bound clamping — the caller's responsible for a sensible
    //    dt; the engine doesn't second-guess a long frame (e.g. unit-
    //    test scenarios deliberately use dt = 1 s).  Copilot PR #13
    //    review caught the missing sanitisation.
    auto finite_or_zero = [](f32 v) noexcept -> f32 {
        return (v == v && v != std::numeric_limits<f32>::infinity()
                       && v != -std::numeric_limits<f32>::infinity()) ? v : 0.0f;
    };
    const f32 dt          = std::max(0.0f, finite_or_zero(in.dt_sec));
    const f32 fwd_axis    = clamp_finite(in.forward_axis, -1.0f, 1.0f);
    const f32 strafe_axis = clamp_finite(in.strafe_axis,  -1.0f, 1.0f);
    const f32 up_axis     = clamp_finite(in.up_axis,      -1.0f, 1.0f);
    const f32 speed       = std::max(0.0f, finite_or_zero(in.move_speed_m_s));
    const f32 yaw_delta   = finite_or_zero(in.yaw_delta_deg);
    const f32 pitch_delta = finite_or_zero(in.pitch_delta_deg);

    // 1) Integrate look. Pitch is sign-flipped when invert_pitch is set.
    const f32 pitch_sign = in.invert_pitch ? -1.0f : 1.0f;
    cam.yaw_deg   = wrap_deg_180(cam.yaw_deg + yaw_delta);
    cam.pitch_deg = clamp_finite(cam.pitch_deg + pitch_sign * pitch_delta,
                                 -89.0f, 89.0f);

    // 2) Defensive clamp on the integrated yaw — wrap_deg_180 already
    //    handles the common case and the NaN case (returns 0); the extra
    //    clamp here is belt-and-braces against a future refactor.
    cam.yaw_deg = clamp_finite(cam.yaw_deg, -180.0f, 180.0f);

    // 3) Move along the yaw-rotated basis. We deliberately use the FULL
    //    basis (which includes pitch tilt on the forward vector) for
    //    forward/strafe — this is the "flying camera" feel that matches
    //    sample-02's needs. Sample 06 (jet-pack) can flip `up_axis` for
    //    vertical thrust.
    //
    //    For an FPS-on-the-ground camera the caller can zero the y
    //    component of forward after the call, or just request
    //    forward_axis=0 + up_axis=0 when standing still.
    const Basis b = basis_from_angles(cam.yaw_deg, cam.pitch_deg);

    // velocity_local = (strafe, up, forward)  in local axes
    // velocity_world = strafe*right + up*world_up + forward*forward
    const math::Vec3 world_up{ 0.0f, 1.0f, 0.0f };
    math::Vec3 vel = math::mul(b.right,   strafe_axis * speed);
    vel = math::add(vel, math::mul(b.forward, fwd_axis * speed));
    vel = math::add(vel, math::mul(world_up,  up_axis  * speed));

    const math::Vec3 dp = math::mul(vel, dt);
    cam.position[0] += dp.x;
    cam.position[1] += dp.y;
    cam.position[2] += dp.z;
}

// ─── Matrix builders ────────────────────────────────────────────────────

math::Mat4 view_matrix(const CameraState& cam) noexcept {
    const math::Vec3 eye = to_vec3(cam.position);
    const Basis b        = basis_from_angles(cam.yaw_deg, cam.pitch_deg);
    const math::Vec3 target = math::add(eye, b.forward);
    const math::Vec3 world_up{ 0.0f, 1.0f, 0.0f };
    return math::look_at_rh(eye, target, world_up);
}

math::Mat4 proj_matrix(const CameraState& cam, f32 viewport_aspect) noexcept {
    // Guard against a degenerate aspect — a zero or negative value would
    // blow up perspective_rh (division by tan*aspect). Force a finite,
    // positive fallback rather than poisoning the matrix.
    f32 aspect = viewport_aspect;
    if (!(aspect == aspect) || aspect <= 0.0f) aspect = 1.0f;

    const f32 fov_rad = deg_to_rad(cam.fov_y_deg);
    return math::perspective_rh(fov_rad, aspect, cam.near_m, cam.far_m);
}

math::Mat4 view_proj_matrix(const CameraState& cam, f32 viewport_aspect) noexcept {
    const math::Mat4 v = view_matrix(cam);
    const math::Mat4 p = proj_matrix(cam, viewport_aspect);
    return math::mul(p, v);
}

// ─── Basis vector accessors ─────────────────────────────────────────────

math::Vec3 forward_vector(const CameraState& cam) noexcept {
    return basis_from_angles(cam.yaw_deg, cam.pitch_deg).forward;
}

math::Vec3 right_vector(const CameraState& cam) noexcept {
    return basis_from_angles(cam.yaw_deg, cam.pitch_deg).right;
}

math::Vec3 up_vector(const CameraState& cam) noexcept {
    return basis_from_angles(cam.yaw_deg, cam.pitch_deg).up_local;
}

}  // namespace psynder::camera
