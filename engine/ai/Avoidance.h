// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Avoidance.h
//
// Local agent-avoidance steering: keep pursuing a desired velocity, but veer to
// dodge neighbours that are about to collide. A simplified, DETERMINISTIC
// time-to-collision / reciprocal-velocity nudge — the local-avoidance layer that
// sits on top of the goal-seeking primitives in Steering.h (seek / arrive /
// flow-field follow give you a desired velocity; avoid_velocity bends it around
// other moving agents before you integrate it). It is the per-agent companion to
// FlowField's shared field: many agents share one field for global pathing, then
// each applies avoid_velocity against its local neighbours so they don't pile up.
//
// Model (see Avoidance.cpp for the algebra):
//   * Each agent is a disc of `radius` on the XZ plane moving at `vel`.
//   * For a (self, neighbour) pair, time_to_collision solves a quadratic for the
//     first instant their expanding circles (separation == r_self + r_other)
//     touch under their CURRENT relative motion — a pure ToC estimate, no trig.
//   * If that time lies within (0, time_horizon_s] the neighbour is a THREAT.
//     We build a repulsive steering contribution that is perpendicular-biased
//     (push sideways off the collision course) plus a separation term (push
//     directly apart), scaled by an URGENCY weight: sooner-to-hit and
//     closer-now neighbours push harder. Already-overlapping pairs get a strong
//     pure-separation shove.
//   * Contributions are summed in span order, added to `desired_vel`, and the
//     whole thing is clamped to `max_speed`. With no threats the result is just
//     `desired_vel` clamped — the goal pursuit is untouched.
//
// XZ-plane convention (matching Steering.h / FlowField): all math is on the XZ
// projection; inputs' y is ignored and every returned vector has y == 0.
// Magnitudes are real speeds in m/s (1 world unit = 1 metre); radii in metres.
//
// Determinism (lockstep pillar): pure +,-,*,/ and guarded sqrt only — no trig /
// acos / RNG / platform branches, and the neighbour span is processed strictly
// in order, so identical inputs yield bit-identical steered velocities on every
// run and platform. Built -fno-fast-math -ffp-contract=off. Every normalize is
// guarded against a zero-length direction (no NaN). Allocation-free.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <span>

namespace psynder::ai {

// One disc agent for avoidance: XZ position, XZ velocity (m/s) and radius (m).
// The y components of `pos` / `vel` are ignored (XZ-plane convention).
struct AvoidAgent {
    math::Vec3 pos{0.0f, 0.0f, 0.0f};
    math::Vec3 vel{0.0f, 0.0f, 0.0f};
    f32        radius = 0.0f;
};

// Sentinel returned by time_to_collision when the two agents will NEVER collide
// under their current relative motion (separating, or parallel and clear). A
// large finite value (not infinity) so callers can compare with `<` against a
// horizon without special-casing inf, and it stays bit-deterministic.
inline constexpr f32 kNoCollision = 1.0e30f;

// (XZ) time in seconds until the two agents' discs first touch — the smallest
// t >= 0 at which |(b.pos - a.pos) + (b.vel - a.vel) * t| == a.radius + b.radius,
// solved as a quadratic in t. Convention:
//   * Already overlapping (separation < combined radii at t == 0): returns a
//     SMALL NEGATIVE value (-1.0f) to flag "collide now / interpenetrating".
//   * Closing onto a future contact: returns that positive contact time.
//   * Separating, or moving apart / parallel so contact never happens: returns
//     the large positive sentinel kNoCollision.
// Pure algebra (one guarded sqrt for the quadratic discriminant); no trig/RNG.
f32 time_to_collision(const AvoidAgent& a, const AvoidAgent& b) noexcept;

// Bend `desired_vel` to avoid neighbours about to collide with `self`, then clamp
// the result to `max_speed`. For each neighbour (processed in span order) we take
// its time_to_collision with `self`; if that contact falls within
// (0, time_horizon_s] — or the pair already overlaps — we add a repulsive,
// perpendicular-biased + separation steering contribution weighted by urgency
// (sooner / closer == stronger). The summed avoidance is added to `desired_vel`
// and the whole vector is capped at `max_speed`. With NO threatening neighbours
// the output is exactly `desired_vel` clamped to `max_speed`. Result y == 0.
// Allocation-free and deterministic.
math::Vec3 avoid_velocity(const AvoidAgent& self, math::Vec3 desired_vel,
                          std::span<const AvoidAgent> neighbours,
                          f32 time_horizon_s, f32 max_speed) noexcept;

}  // namespace psynder::ai
