// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Perception.h — a vision/perception sensor: can an observer with a
// facing direction, a vision cone (FOV) and a max range perceive a target, and
// how strongly. The angular complement to PathSimplify's grid line_of_sight: use
// this to gate "is the target in my view cone + range", then line_of_sight to
// gate "is the view unobstructed".
//
// Uses a PRECOMPUTED cosine threshold (fov_cos_half_angle), never an angle, so
// the per-tick path is lockstep-safe: pure +,-,*,/ and one sqrt for normalize —
// no acos / runtime trig / RNG. Full 3D distance + 3D facing (a flying/aiming
// observer perceives in 3D); flatten the inputs to the XZ plane at the call site
// for a ground-only cone. Determinism: same inputs => bit-identical result.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <span>

namespace psynder::ai {

// True iff `target_pos` is within `range_m` of `observer_pos` AND inside the
// view cone: dot(normalize(facing), normalize(target-observer)) >=
// fov_cos_half_angle. A target AT the observer is perceivable; a degenerate
// (zero-length) facing makes the cone omnidirectional within range. Lockstep-
// safe (cosine compare, no acos).
bool can_perceive(math::Vec3 observer_pos, math::Vec3 observer_facing,
                  math::Vec3 target_pos, f32 fov_cos_half_angle,
                  f32 range_m) noexcept;

// Graded perception in [0,1]: 0 when not perceivable (out of range or outside
// the cone), otherwise distance_factor * angle_factor where distance_factor =
// 1 - d/range (1 at the observer, 0 at the range edge) and angle_factor remaps
// the cosine over [fov_cos_half_angle, 1] to [0,1] (1 dead-ahead, 0 at the cone
// edge). Highest for a close, centered target. Pure algebra.
f32 perception_strength(math::Vec3 observer_pos, math::Vec3 observer_facing,
                        math::Vec3 target_pos, f32 fov_cos_half_angle,
                        f32 range_m) noexcept;

// SETUP/authoring helper: cos(half_angle_deg). Uses std::cos, so it is NOT
// guaranteed bitwise-identical across platforms — convert ONCE offline / at
// authoring and pass the cosine to the per-tick queries above (mirrors how the
// weapon lanes pass a precomputed spread_tan / cone cosine).
f32 fov_cos(f32 half_angle_deg) noexcept;

// Index of the most-perceptible target (highest perception_strength > 0), or -1
// if none is perceivable. Ties break to the lowest index. Deterministic.
i32 most_perceptible(math::Vec3 observer_pos, math::Vec3 facing,
                     std::span<const math::Vec3> targets,
                     f32 fov_cos_half_angle, f32 range_m) noexcept;

}  // namespace psynder::ai
