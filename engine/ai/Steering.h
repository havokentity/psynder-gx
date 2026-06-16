// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Steering.h
//
// The classic Reynolds steering primitives ("Steering Behaviors For Autonomous
// Characters", Reynolds 1999) as PURE functions over Vec3 — seek, flee, arrive,
// plus the two composition helpers clamp_speed and blend. Each returns a DESIRED
// VELOCITY (a direction scaled by a speed in m/s), so an agent can compute one,
// or blend several, then integrate against its own kinematics. These are the
// stateless building blocks beneath NavAgent / PathFollow / FlowField: where
// FlowField gives a shared unit direction and PathFollow walks a route, these
// give a single agent its instantaneous pull toward (or away from) a point.
//
// XZ-plane convention (matching FlowField / PathFollow): all steering happens in
// the world XZ plane and every returned vector has y == 0. The y components of
// the inputs are ignored — only the XZ projection of pos / target / threat /
// velocity is used — so an agent at a different height than its target still
// steers as if both lie on the ground plane. Magnitudes are real speeds in m/s
// (1 world unit = 1 metre), not unit vectors: |seek(...)| == max_speed when not
// already at the target.
//
// Determinism (lockstep pillar): pure +,-,*,/ and at most one guarded sqrt per
// call (for the normalize / magnitude). No trig / acos / RNG / platform
// branches, so identical inputs yield bit-identical outputs on every run and
// platform. Built -fno-fast-math -ffp-contract=off. Every normalize is guarded:
// a zero-length direction returns the zero vector rather than producing a NaN.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

namespace psynder::ai {

// Desired velocity that drives `pos` straight TOWARD `target` at `max_speed`
// (m/s): normalize(target - pos) * max_speed, projected onto XZ with y == 0.
// Returns the zero vector when `pos` already coincides with `target` on the XZ
// plane (zero-length direction guarded — no NaN).
math::Vec3 seek(math::Vec3 pos, math::Vec3 target, f32 max_speed) noexcept;

// Desired velocity that drives `pos` straight AWAY from `threat` at `max_speed`
// (m/s): normalize(pos - threat) * max_speed, projected onto XZ with y == 0.
// Returns the zero vector when `pos` is exactly at `threat` on the XZ plane
// (no well-defined flee direction — guarded, no NaN).
math::Vec3 flee(math::Vec3 pos, math::Vec3 threat, f32 max_speed) noexcept;

// Like seek, but eases onto the target instead of overshooting: outside
// `slow_radius_m` the desired speed is the full `max_speed`; inside it the speed
// ramps DOWN linearly with distance — speed = max_speed * (dist / slow_radius_m),
// clamped to [0, max_speed] — so the agent decelerates as it closes in. Returns
// the zero vector within a tiny arrival epsilon of the target (and when
// `slow_radius_m` <= 0, which degenerates to a hard stop at the target). The
// direction is the XZ unit vector toward `target`; result y == 0.
math::Vec3 arrive(math::Vec3 pos, math::Vec3 target, f32 max_speed,
                  f32 slow_radius_m) noexcept;

// Speed limiter: if the XZ magnitude of `velocity` exceeds `max_speed`, scale it
// down to exactly `max_speed` (preserving direction); otherwise return it
// unchanged. The returned vector has y == 0 (XZ projection). A zero (or
// sub-`max_speed`) velocity is returned as-is, with y zeroed. Guards the
// zero-length case (no NaN).
math::Vec3 clamp_speed(math::Vec3 velocity, f32 max_speed) noexcept;

// Weighted composition of two steering outputs: a * wa + b * wb, projected onto
// XZ with y == 0. Lets an agent mix behaviors (e.g. seek the goal while fleeing a
// threat) before clamp_speed caps the result. Pure algebra; no normalize.
math::Vec3 blend(math::Vec3 a, math::Vec3 b, f32 wa, f32 wb) noexcept;

}  // namespace psynder::ai
