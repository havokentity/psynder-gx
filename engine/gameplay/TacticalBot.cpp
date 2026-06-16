// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/TacticalBot.cpp — the tactical FSM decision. See TacticalBot.h.

#include "gameplay/TacticalBot.h"

#include <algorithm>  // std::clamp

namespace psynder::gameplay {

TacticalState decide_tactical_state(f32 hp, f32 max_hp, bool enemy_visible,
                                    f32 retreat_frac) noexcept {
    const f32 frac = std::clamp(retreat_frac, 0.0f, 1.0f);
    // Survival first: a hurt bot falls back to cover regardless of a visible
    // enemy. Guard a degenerate health pool (max_hp <= 0) so it never traps the
    // bot in Retreat.
    if (max_hp > 0.0f && hp <= frac * max_hp) {
        return TacticalState::Retreat;
    }
    if (enemy_visible) {
        return TacticalState::Engage;
    }
    return TacticalState::Patrol;
}

}  // namespace psynder::gameplay
