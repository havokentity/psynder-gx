// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/CombatBot.h — deterministic AI combatants: path via the
// flow field, shoot in-range enemies through the weapon systems.

#pragma once

#include "ai/FlowField.h"

#include "scene/World.h"

#include "core/Types.h"

namespace psynder::gameplay {

// Team affiliation; bots only target other teams.
PSYNDER_COMPONENT(Team) {
    u32 team;
};
static_assert(sizeof(Team) == 4, "Team layout frozen");

// Marks an AI-controlled combatant.
PSYNDER_COMPONENT(Bot) {
    f32 fire_range_m;  // engage enemies within this distance
    f32 lookahead_m;   // how far ahead along the flow field to aim its movement
};
static_assert(sizeof(Bot) == 8, "Bot layout frozen");

// Drive AI combatants for one tick: each bot (Bot + Team + Health(alive) +
// Weapon + scene::AgentTarget + TransformWS) steers along the flow field toward
// the goal (writes its AgentTarget a lookahead step along the sampled flow
// direction) and, if the nearest live enemy of another team is within
// fire_range, fires its hitscan weapon at it (which gates on cooldown/ammo and
// credits the kill). Deterministic: bots processed in ascending entity-id order;
// the nearest-enemy tie is broken by lower id. (Movement is then resolved by the
// agents steering system; call tick_weapons + this each tick.)
void tick_combat_bots(scene::World& w, const ai::FlowField& field, f32 dt_seconds);

}  // namespace psynder::gameplay
