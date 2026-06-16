// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/SquadCommand.cpp — see SquadCommand.h for the contract.

#include "ai/SquadCommand.h"

namespace psynder::ai {

void squad_set_order(SquadState& s, SquadOrder order,
                     math::Vec3 rally_point) noexcept {
    s.order = order;
    s.rally_point = rally_point;
}

math::Vec3 squad_member_goal(const SquadState& s, math::Vec3 leader_pos,
                             math::Vec3 leader_forward, u32 member_index,
                             f32 spacing_m) noexcept {
    switch (s.order) {
        case SquadOrder::Advance:
            // Wedge anchored at the rally point, facing the leader's heading:
            // the squad advances in formation toward the objective.
            return formation_slot(s.rally_point, leader_forward,
                                  FormationShape::Wedge, member_index,
                                  spacing_m);
        case SquadOrder::Hold:
            // Wedge anchored at the leader's current spot: hold formation here.
            return formation_slot(leader_pos, leader_forward,
                                  FormationShape::Wedge, member_index,
                                  spacing_m);
        case SquadOrder::Regroup:
        default:
            // Collapse onto the leader.
            return leader_pos;
    }
}

bool squad_all_arrived(const SquadState& s,
                       std::span<const math::Vec3> member_positions,
                       math::Vec3 leader_pos, math::Vec3 leader_forward,
                       f32 spacing_m, f32 arrive_radius_m) noexcept {
    const f32 r2 = arrive_radius_m * arrive_radius_m;
    for (usize i = 0; i < member_positions.size(); ++i) {
        // Span entry i is a non-leader member => formation slot i+1 (slot 0 is
        // the leader's own slot).
        const u32 slot = static_cast<u32>(i) + 1u;
        const math::Vec3 goal = squad_member_goal(s, leader_pos, leader_forward,
                                                  slot, spacing_m);
        const math::Vec3 p = member_positions[i];
        const f32 dx = p.x - goal.x;
        const f32 dz = p.z - goal.z;  // XZ-plane distance (y ignored)
        if (dx * dx + dz * dz > r2) {
            return false;
        }
    }
    return true;
}

}  // namespace psynder::ai
