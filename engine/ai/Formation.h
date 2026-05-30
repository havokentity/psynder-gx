// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Formation.h
//
// Deterministic squad formation slots — the cohesion complement to per-agent
// nav (FlowField / steering). Given a squad leader's world position and facing,
// formation_slot() maps a member index to that member's target world slot, for a
// handful of classic shapes (Line abreast, Column single-file, Wedge V). Members
// can then steer toward their slot to hold formation while the squad moves.
//
// Convention (XZ-plane, matches FlowField.h and engine/camera/Camera.h):
//   - World up is +Y; slots all live in the leader's XZ plane (y == leader_pos.y).
//   - The right axis is right = normalize(cross(forward, up)) with up = +Y.
//     This matches Camera.h's basis (Camera.cpp right_vector): for the canonical
//     look forward = (0,0,-1) the right axis is (+1,0,0), and a forward of
//     (1,0,0) (looking down +X) yields right = (0,0,1). NOTE: this is
//     cross(forward, up), not cross(up, forward) — in this right-handed,
//     -Z-forward (OpenGL/GL-style) convention cross(up, forward) would point
//     LEFT, so we take cross(forward, up) to get +X on the canonical look, the
//     handedness the rest of the engine documents and uses.
//   - A degenerate (zero-length) forward falls back to forward = (0,0,-1).
//
// Determinism (lockstep pillar): pure vector algebra plus the single sqrt inside
// math::normalize — no RNG, no trig. Same inputs => bit-identical slots. Built
// under the lane's -fno-fast-math / -ffp-contract=off flags.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

// Squad formation shapes. Member index 0 is always the leader's own slot.
enum class FormationShape : u32 {
    Line = 0,    // members abreast, alternating right then left of the leader
    Column = 1,  // single file directly behind the leader (along -forward)
    Wedge = 2,   // a V trailing behind, fanning out to alternating sides
};

// World slot for `member_index` in a `shape` formation anchored at `leader_pos`
// facing `leader_forward`, with `spacing_m` metres between adjacent ranks/files.
//
//   member_index == 0  => leader's own slot (== leader_pos) for every shape.
//
//   Line   : 1 -> +1 right, 2 -> -1 right (left), 3 -> +2 right, 4 -> -2 right,
//            ... alternating right/left along the right axis at spacing steps.
//   Column : member i sits i*spacing behind the leader (along -forward).
//   Wedge  : members fan out behind AND to alternating sides; member i steps
//            back by ceil(i/2)*spacing and out by ceil(i/2)*spacing, the side
//            alternating right (odd i) / left (even i).
//
// All slots lie in the leader's XZ plane (y == leader_pos.y). A zero-length
// `leader_forward` is treated as (0,0,-1).
math::Vec3 formation_slot(math::Vec3     leader_pos,
                          math::Vec3     leader_forward,
                          FormationShape shape,
                          u32            member_index,
                          f32            spacing_m) noexcept;

// Fill out[0..count) with formation_slot() for indices 0..count-1. Resizes
// `out` to exactly `count`; out[i] == formation_slot(.., i, ..).
void formation_slots(math::Vec3              leader_pos,
                     math::Vec3              leader_forward,
                     FormationShape          shape,
                     u32                     count,
                     f32                     spacing_m,
                     std::vector<math::Vec3>& out) noexcept;

}  // namespace psynder::ai
