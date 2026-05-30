// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/TacticalBot.h — deterministic high-level tactical FSM for AI
// combatants. It sits ABOVE the concrete subsystems a bot drives — navigation
// (ai::NavAgent / FlowField), weapons (fire_hitscan + RangedDamage falloff),
// and cover (ai::CoverPoints) — and decides WHAT a bot should do this tick from
// just its health and whether it can currently see an enemy: hold and shoot,
// fall back to cover, or advance toward the objective. The caller wires the
// returned action to the concrete systems (steer to a goal cell, fire a weapon).
//
// Keeping the decision a pure function (no World, no RNG, no allocation) makes
// it trivially unit-testable AND lockstep-safe: the same (health, visibility)
// inputs yield the same state on every peer. The gameplay lane is strict-FP, so
// the single health-fraction comparison is bit-identical across platforms.

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "core/Types.h"

namespace psynder::gameplay {

// A bot's high-level tactical action for the current tick.
enum class TacticalState : u32 {
    Patrol  = 0,  // no visible threat — advance toward the objective
    Engage  = 1,  // healthy with a visible enemy — hold position and shoot
    Retreat = 2,  // hurt below the threshold — fall back to the nearest cover
};

// Per-bot tactical state + its retreat threshold (as a fraction of max health).
// POD; `state` stores a TacticalState as u32 so it rides in ECS storage without
// padding surprises.
PSYNDER_COMPONENT(TacticalBot) {
    u32 state;                // current TacticalState
    f32 retreat_health_frac;  // retreat when hp <= retreat_health_frac * max_hp
};
static_assert(sizeof(TacticalBot) == 8, "TacticalBot layout frozen");

// Decide the tactical action from health + enemy visibility. Fight-or-flight:
//   * a bot at or below retreat_frac * max_hp RETREATS, even with a visible
//     enemy (survival comes first);
//   * otherwise a visible enemy means ENGAGE;
//   * with no visible enemy the bot PATROLs toward the objective.
// Pure: no RNG, no allocation, no platform branches. retreat_frac is clamped to
// [0, 1]; a non-positive max_hp is treated as "not hurt" (never forces Retreat
// on a degenerate health pool). Same inputs => same TacticalState everywhere.
TacticalState decide_tactical_state(f32 hp, f32 max_hp, bool enemy_visible,
                                    f32 retreat_frac) noexcept;

}  // namespace psynder::gameplay
