// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Flank.h
//
// Deterministic flanking-position computation: where should an attacker move to
// engage a target from the SIDE or REAR, out of the target's front arc? Given a
// target's world position and facing, flank_position() returns a world point a
// chosen distance away on the target's left, right, or rear; best_flank_side()
// picks the side that needs the least repositioning for a given attacker; and
// in_front_arc() tests whether a point sits inside the target's forward cone (so
// a good flank position FAILS it — the target cannot see the attacker coming).
//
// The attacker then steers (engine/ai/Steering.h seek/arrive) toward the flank
// position to gain a positional advantage over a target whose attention faces
// the wrong way.
//
// Convention (XZ-plane, matches Formation.h / Steering.h / TargetSelect.h):
//   - World up is +Y; every flank position lies in the target's XZ plane
//     (y == target_pos.y). The y component of `target_facing` is ignored.
//   - forward = normalize(target_facing flattened to XZ), with a (0,0,-1)
//     fallback for a degenerate (zero-length horizontal) facing.
//   - right = (-fwd.z, 0, fwd.x) == cross(forward, +Y) (Formation.h's
//     handedness): forward (0,0,-1) -> right (+1,0,0); forward (1,0,0) ->
//     right (0,0,1).
//   - Offsets (each scaled by distance_m): Right -> +right, Left -> -right,
//     Rear -> -forward.
//
// Determinism (lockstep pillar): pure +,-,*,/ plus the single guarded sqrt
// inside math::normalize — no RNG, no trig, no acos (front-arc uses a precomputed
// cosine). Same inputs => bit-identical output on every platform. Built under the
// lane's -fno-fast-math / -ffp-contract=off flags.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

namespace psynder::ai {

// Which side of the target to approach from. Ordered so deterministic ties in
// best_flank_side() break Left < Right < Rear.
enum class FlankSide : u32 {
    Left = 0,   // offset = -right
    Right = 1,  // offset = +right
    Rear = 2,   // offset = -forward
};

// World position `distance_m` metres off the target on the chosen `side`, in the
// target's XZ plane (y == target_pos.y). Builds the target's XZ basis from
// `target_facing` (degenerate facing falls back to forward (0,0,-1)) and returns
// target_pos + offset * distance_m, where offset is +right (Right), -right
// (Left), or -forward (Rear).
math::Vec3 flank_position(math::Vec3 target_pos, math::Vec3 target_facing,
                          FlankSide side, f32 distance_m) noexcept;

// The flank side whose unit-distance position is CLOSEST to `attacker_pos`, i.e.
// the side that needs the least repositioning. Deterministic ties break in
// Left < Right < Rear order (the FlankSide enum order). The y components of the
// inputs are ignored (comparison is on the XZ plane).
FlankSide best_flank_side(math::Vec3 attacker_pos, math::Vec3 target_pos,
                          math::Vec3 target_facing) noexcept;

// True iff `point` sits inside the target's front cone: with
// dir = normalize(point - target_pos) flattened to XZ, returns
// dot(forward, dir) >= front_cos_half_angle. `front_cos_half_angle` is the
// PRECOMPUTED cosine of the cone's half-angle (e.g. 0.5 for a 120-degree total
// cone), so no acos is needed. A point coincident with the target on the XZ
// plane (zero-length direction) is treated as NOT in the arc (returns false). A
// well-chosen flank position should make this false — the target cannot see it.
bool in_front_arc(math::Vec3 target_pos, math::Vec3 target_facing,
                  math::Vec3 point, f32 front_cos_half_angle) noexcept;

}  // namespace psynder::ai
