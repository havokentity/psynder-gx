// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Orbit.h
//
// Lane 16 — third-person orbit / chase camera. Positions a camera on a sphere
// around a target (the player) given yaw, pitch, and a distance radius, then
// reports where the eye sits and which way it looks. The standard
// over-the-shoulder / chase camera that an FPS engine flips to for vehicles,
// death cams, spectate, or a toggle-to-third-person view.
//
// This is purely additive: it computes a camera POSITION + look direction from
// orbit parameters; it does NOT replace Camera.h's CameraState — the typical
// caller writes the returned yaw/pitch + an eye position into a CameraState and
// lets the existing view_matrix() path build the matrix. The orbit module owns
// none of that; it is a stateless data transform on caller-owned PODs.
//
// ── Coordinate / angle convention (MUST match Camera.h exactly) ──────────────
//   - Right-handed, world-up is +Y, world-forward at (yaw=0, pitch=0) is -Z
//     (the OpenGL / Vulkan look-down-negative-Z default).
//   - Yaw is rotation around +Y in DEGREES; pitch is rotation around the
//     camera's local right axis in DEGREES, positive pitch looks UP (toward +Y).
//   - The look direction the orbit produces uses the SAME formula Camera.cpp's
//     basis_from_angles() uses, so an orbit camera and an FPS camera fed the
//     same yaw/pitch agree on where they are pointed:
//
//         forward = ( cos(pitch) * sin(yaw),
//                     sin(pitch),
//                    -cos(pitch) * cos(yaw) )
//
//     Sanity:
//         yaw=0,   pitch=0   → forward = ( 0, 0, -1)   (canonical -Z look)
//         yaw=+90, pitch=0   → forward = (+1, 0,  0)
//         yaw=0,   pitch=+30 → forward = ( 0, +sin30, -cos30)  (look up)
//
//   - The camera is a CHASE camera: it looks TOWARD the target, and the eye is
//     placed `distance_m` BEHIND the target along that look direction. So:
//
//         eye = target - forward * distance_m
//
//     With forward at yaw=0/pitch=0 being (0,0,-1), the eye lands at
//     target + (0, 0, +distance) — directly behind along +Z — and looks back
//     down -Z at the target. A positive pitch (look up) tilts the forward toward
//     +Y, which subtracts a +Y component from the eye via `-forward*distance`
//     → the eye RISES above the target while the look stays pointed at it (a
//     high-angle chase shot). 1 world unit = 1 metre.
//
//   - The returned `yaw_deg` / `pitch_deg` are exactly the inputs (the camera
//     faces the target along the orbit's own yaw/pitch), ready to drop into a
//     CameraState.
//
// ── Determinism ──────────────────────────────────────────────────────────────
// This is the camera-POSITIONING lane: cosmetic / view-only, NOT the lockstep
// simulation. orbit_eye() uses std::sin / std::cos, so it is bit-identical only
// across runs of the SAME platform / libm (macOS libm, glibc, and MSVCRT
// disagree at sub-ulp), matching the same-platform guarantee documented in
// Camera.h. That is sufficient here — no game-state, raycast, or replay hash
// depends on the chase-camera eye position, only what the local player sees.
// orbit_rotate() / orbit_zoom() are pure clamp/wrap arithmetic with no trig and
// are bit-identical everywhere. All three functions are pure: no RNG, no
// wall-clock, no globals, no hidden state.

#pragma once

#include "core/Types.h"

namespace psynder::camera {

// ── Orbit parameters (POD) ───────────────────────────────────────────────────
// The point being orbited (the player / target) + the orbit angles + the radius.
// Caller owns the storage; trivially-copyable so it can live in an ECS chunk.
struct OrbitState {
    f32 target[3]{0.0f, 0.0f, 0.0f};  // metres, world space — the orbit centre
    f32 yaw_deg    = 0.0f;            // around +Y, [-180, 180] when integrated
    f32 pitch_deg  = 0.0f;            // around local right, clamped [-89, 89]
    f32 distance_m = 4.0f;            // orbit radius (eye-to-target distance)
};

// ── Orbit result (POD) ───────────────────────────────────────────────────────
// Where the eye sits, the UNIT look direction toward the target, and the
// yaw/pitch to write straight into a CameraState. `forward` equals
// normalize(target - eye) — the camera looks back at the orbit centre.
struct OrbitResult {
    f32 eye[3]{0.0f, 0.0f, 0.0f};      // metres, world space — camera position
    f32 forward[3]{0.0f, 0.0f, -1.0f}; // unit look direction (toward target)
    f32 yaw_deg   = 0.0f;              // == OrbitState::yaw_deg
    f32 pitch_deg = 0.0f;              // == OrbitState::pitch_deg
};

// Place the eye `distance_m` BEHIND the target along the yaw/pitch look
// direction, so the camera looks at the target from behind (the chase cam):
//
//     forward = ( cos(pitch)*sin(yaw), sin(pitch), -cos(pitch)*cos(yaw) )
//     eye     = target - forward * distance_m
//
// (See the file header for the full derivation + sanity values.) The returned
// `forward` is the unit vector FROM the eye TOWARD the target, and the returned
// yaw/pitch equal the inputs. Pure; same-platform strict-FP (uses trig).
OrbitResult orbit_eye(const OrbitState& s) noexcept;

// Add to the orbit angles: yaw accumulates and WRAPS into [-180, 180] (matching
// Camera.h's yaw integration); pitch accumulates and CLAMPS into [-89, 89] (no
// gimbal flip past straight up/down, matching Camera.h's pitch clamp). Non-
// finite deltas are ignored (treated as 0) so a bad input frame can't poison the
// orbit. Pure clamp/wrap arithmetic — no trig, bit-identical everywhere.
void orbit_rotate(OrbitState& s, f32 yaw_delta_deg, f32 pitch_delta_deg) noexcept;

// Adjust the orbit radius by `delta_m` (e.g. scroll-wheel zoom), clamped into
// [min_dist, max_dist]. A non-finite delta is ignored. If the supplied range is
// degenerate (min > max) it is normalised by swapping so the clamp still yields
// a value inside the intended band. Pure; bit-identical everywhere.
void orbit_zoom(OrbitState& s, f32 delta_m, f32 min_dist, f32 max_dist) noexcept;

}  // namespace psynder::camera
