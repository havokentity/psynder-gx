// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Camera.h
//
// Lane 16 — Camera + Input glue. Turns lane 25's raw per-frame deltas + lane
// 02's math into a per-frame view / projection matrix the render pipeline can
// consume.
//
// Design (DOTS-friendly):
//   - CameraState is a POD struct owned by the CALLER.  No globals, no
//     singletons.  The API is stateless aside from the caller-owned
//     CameraState reference — `update_camera` mutates the passed-in struct
//     in place; `view_matrix` / `proj_matrix` / `view_proj_matrix` only
//     read it.  No hidden side effects, no statics, no thread-local
//     scratch.  This makes multi-camera / split-screen / multi-view
//     trivially parallel — drive N CameraStates from N JobSystem workers.
//   - No virtual, no RTTI, no std::shared_ptr. Trivially-copyable.
//   - Per-frame input is FILTERED, normalized deltas (not raw NSEvent /
//     XInput / Win32-WM). The caller (sample or game code) is responsible
//     for the platform poll + the sensitivity multiply + the dt clock.
//
// Conventions (Psynder-GX project-wide):
//   - Right-handed, column-major matrices (matches engine/math/).
//   - World-up axis is +Y. World forward (yaw=0, pitch=0) is -Z. This
//     matches the OpenGL / Vulkan "look-down-negative-Z" default and the
//     existing look_at_rh / perspective_rh helpers in engine/math/Math.cpp.
//   - Yaw is rotation around the world +Y axis, in DEGREES, range
//     [-180, +180]. Wraps when integrated.
//       yaw =   0°  → forward = (0, 0, -1)   (canonical -Z look)
//       yaw = +90°  → forward = (+1, 0, 0)   (looking down +X)
//       yaw = -90°  → forward = (-1, 0, 0)
//       yaw = ±180° → forward = (0, 0, +1)
//     i.e. forward = ( +sin(yaw), 0, -cos(yaw) ) at pitch=0.
//   - Pitch is rotation around the camera's local right axis, in DEGREES,
//     CLAMPED to [-89, +89] (no gimbal flip, no looking straight up/down).
//       Positive pitch tilts the look-vector toward +Y (look up).
//   - 1 world-unit = 1 metre.  move_speed_m_s is metres-per-second of
//     world-space velocity along the FULL yaw+pitch-rotated basis (the
//     "flying camera" model — forward gains a Y component when looking
//     up/down).  For an FPS-on-the-ground feel where forward stays
//     horizontal, the caller can zero `up_axis` and ground-clamp
//     `position[1]` after the call.  Sample 06 (jet-pack) intentionally
//     keeps the flying-camera path.
//
// Determinism: this TU is compiled with `-fno-fast-math -ffp-contract=off`
// (Clang/GCC) or `/fp:strict` (MSVC) so the view+proj matrices are bit-
// identical across runs of the SAME platform / libm / STL combo.  Cross-
// platform bit-identity is NOT claimed — `std::sin` / `std::cos` /
// `std::tan` are implementation-defined and macOS libm, glibc, and MSVCRT
// disagree at sub-ulp precision (Copilot PR #13 review).  Lockstep
// determinism for true cross-host replay would need a project-owned
// polynomial trig kernel; that's M5 work paired with the demo-replay
// validator.  The current guarantee — same-platform bit-identical — is
// sufficient for in-session frustum culling + raycast determinism.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

namespace psynder::camera {

// ─── Per-camera persistent state (POD) ───────────────────────────────────
// Caller owns the struct lifetime and storage. Trivially-copyable so it can
// be memcpy'd into GPU constant buffers if the renderer ever wants to.
struct CameraState {
    f32 position[3]{0.0f, 0.0f, 0.0f};   // metres, world space
    f32 yaw_deg   = 0.0f;                // around +Y axis, [-180, 180]
    f32 pitch_deg = 0.0f;                // clamped [-89, 89]
    f32 fov_y_deg = 60.0f;
    f32 near_m    = 0.05f;
    f32 far_m     = 1000.0f;
};

// Layout assertion — `position[3]` + 5 floats = 8 floats = 32 bytes with the
// default alignment of f32 = 4. The orchestrator's task brief suggested
// `sizeof == 28`; that would require sizeof(float[3]) + 4 floats = 7
// floats = 28 bytes. We carry FIVE trailing floats (yaw, pitch, fov, near,
// far) so 32 bytes is the correct value. The static_assert below enforces
// the actual layout the rest of the engine depends on.
static_assert(sizeof(CameraState) == 32, "CameraState POD layout drifted");
static_assert(alignof(CameraState) <= 4, "CameraState should not over-align");

// ─── Per-frame input deltas (POD) ────────────────────────────────────────
// Already-filtered axes + already-integrated mouse deltas. The caller does
// the platform poll, the sensitivity multiply, and the dt clock; the camera
// is a pure data-transform layer.
struct CameraInput {
    f32  forward_axis     = 0.0f;   // -1..+1 (W/S or left-stick Y)
    f32  strafe_axis      = 0.0f;   // -1..+1 (A/D or left-stick X)
    f32  up_axis          = 0.0f;   // -1..+1 (Q/E or jet-pack)
    f32  yaw_delta_deg    = 0.0f;   // already (mouse_x * sensitivity)
    f32  pitch_delta_deg  = 0.0f;   // already (mouse_y * sensitivity)
    f32  dt_sec           = 0.0f;   // last frame's wall-clock delta
    f32  move_speed_m_s   = 5.0f;   // velocity along forward/right/up basis
    bool invert_pitch     = false;  // for users who want "plane sim" pitch
};

// ─── Update / matrix builders (stateless aside from CameraState) ────────
// Integrate yaw/pitch from per-frame deltas (with clamp + wrap), then move
// position along the FULL yaw+pitch-rotated forward/right/up basis (flying-
// camera feel — forward gains a Y component when pitching up/down).  See
// the file header's Conventions block for the FPS-on-the-ground variation.
// Position grows by `velocity_local_rotated * dt`.
//
// All inputs are sanitised against NaN/Inf: non-finite axes / deltas /
// speed become 0; non-finite dt becomes 0 (no integration this frame).
//
// NOT thread-safe on a single CameraState, but a different CameraState per
// thread is perfectly safe — that's the multi-view path.
void update_camera(CameraState& cam, const CameraInput& in) noexcept;

// Right-handed look-at along the yaw+pitch-rotated forward vector, with
// world-Y up. Equivalent to math::look_at_rh(pos, pos+forward, world_up).
math::Mat4 view_matrix(const CameraState& cam) noexcept;

// Right-handed perspective using cam.fov_y_deg / aspect / near_m / far_m.
// `viewport_aspect` is width / height of the render target.
math::Mat4 proj_matrix(const CameraState& cam, f32 viewport_aspect) noexcept;

// Composed view-projection. Equivalent to `mul(proj, view)`.
math::Mat4 view_proj_matrix(const CameraState& cam, f32 viewport_aspect) noexcept;

// ─── Convenience: forward / right / up basis vectors ─────────────────────
// Returned in world space. Useful for hookup code that wants to spawn a
// projectile along the look direction without rebuilding the view matrix.
math::Vec3 forward_vector(const CameraState& cam) noexcept;
math::Vec3 right_vector  (const CameraState& cam) noexcept;
math::Vec3 up_vector     (const CameraState& cam) noexcept;

}  // namespace psynder::camera
