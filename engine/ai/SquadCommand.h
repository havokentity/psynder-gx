// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/SquadCommand.h
//
// Squad-level orders and the per-member goals they produce — the command layer
// above per-agent nav (FlowField / steering) and formation cohesion. A squad
// carries one SquadOrder plus a rally point; squad_member_goal() turns that
// order, the leader's current pose, and a member's index into the world point
// that member should steer toward. Composes ai::Formation (Wedge) for the
// in-position offsets so an advancing squad fans out in a V toward its objective.
//
// Convention (XZ-plane, matches Formation.h / FlowField.h):
//   - World up is +Y; goals live in the leader's (or rally's) XZ plane.
//   - Member indexing: index 0 is the LEADER's own formation slot, so an
//     ordinary squad member with member_index i maps to formation slot i+1
//     (its own non-leader slot). squad_member_goal() takes the raw formation
//     slot index it is given; the arrival check below applies the +1 offset.
//
// Orders:
//   - Advance : members move to their Formation slot (Wedge) anchored at the
//     RALLY POINT, facing leader_forward — the squad advances in wedge to the
//     objective. Each member trails behind / out from the rally per its slot.
//   - Hold    : members hold their Formation slot (Wedge) anchored at the
//     LEADER's CURRENT position — stay in formation where they currently are.
//   - Regroup : every member's goal is the LEADER's position — the squad
//     collapses onto the leader regardless of slot.
//
// Determinism (lockstep pillar): pure vector algebra plus the single sqrt that
// ai::formation_slot() uses internally — no RNG, no trig. Same inputs =>
// bit-identical goals. Built under the lane's -fno-fast-math /
// -ffp-contract=off flags.

#pragma once

#include "ai/Formation.h"

#include "math/Math.h"

#include "core/Types.h"

#include <span>

namespace psynder::ai {

// What a squad is currently doing. See the header comment for per-order goals.
enum class SquadOrder : u32 {
    Advance = 0,  // move to Wedge slots anchored at the rally point
    Hold = 1,     // hold Wedge slots anchored at the leader's current position
    Regroup = 2,  // collapse onto the leader
};

// A squad's current command: an order plus the rally point the order may use
// (only Advance reads rally_point; Hold/Regroup ignore it).
struct SquadState {
    SquadOrder order = SquadOrder::Hold;
    math::Vec3 rally_point{0.0f, 0.0f, 0.0f};
};

// Set the squad's order and rally point.
void squad_set_order(SquadState& s, SquadOrder order,
                     math::Vec3 rally_point) noexcept;

// World goal for `member_index` under the squad's current order, given the
// leader's current `leader_pos`/`leader_forward` and a Wedge `spacing_m`:
//
//   Advance : formation_slot(rally_point, leader_forward, Wedge, member_index, sp)
//   Hold    : formation_slot(leader_pos,  leader_forward, Wedge, member_index, sp)
//   Regroup : leader_pos
//
// `member_index` is a raw Formation slot index: index 0 is the leader's own
// slot (Advance => rally_point, Hold => leader_pos), index 1.. are member slots.
math::Vec3 squad_member_goal(const SquadState& s, math::Vec3 leader_pos,
                             math::Vec3 leader_forward, u32 member_index,
                             f32 spacing_m) noexcept;

// True iff every member in `member_positions` is within `arrive_radius_m` (XZ
// distance) of its squad goal. Member i (0-based in the span) is a non-leader
// squad member and so uses formation slot index i+1 — slot 0 is reserved for
// the leader. An empty `member_positions` returns true (nobody to wait on).
bool squad_all_arrived(const SquadState& s,
                       std::span<const math::Vec3> member_positions,
                       math::Vec3 leader_pos, math::Vec3 leader_forward,
                       f32 spacing_m, f32 arrive_radius_m) noexcept;

}  // namespace psynder::ai
