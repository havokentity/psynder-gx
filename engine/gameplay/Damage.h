// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Damage.h — deterministic damage + death/respawn systems.
//
// On the authoritative lockstep tick: no RNG, stable entity-id processing order,
// no per-frame heap alloc beyond a reused gather buffer. Weapons (hitscan /
// projectile) call apply_damage; the sim calls update_respawns each tick.

#pragma once

#include "scene/World.h"

#include "core/Types.h"

#include <vector>

namespace psynder::gameplay {

// Quake3-style armor ratio: armor absorbs up to 2/3 of incoming damage.
inline constexpr f32 kArmorAbsorbFraction = 2.0f / 3.0f;
// Fallback respawn delay (seconds) when an entity has no Respawnable.
inline constexpr f32 kDefaultRespawnDelay = 3.0f;

// Apply `amount` damage to entity `e` (needs a Health component). Armor (if
// present) absorbs up to kArmorAbsorbFraction of the damage, capped by its
// points; the remainder hits health. On lethal damage health clamps to 0 and a
// Dead tag is added (delay from Respawnable, else kDefaultRespawnDelay).
// Deterministic; returns true iff this damage killed the entity.
bool apply_damage(scene::World& w, Entity e, f32 amount) noexcept;

// Advance every Dead entity's respawn timer by dt; on expiry restore full
// health, move it to its Respawnable spawn (if it has a TransformWS), and clear
// Dead. Deterministic: timers tick in place; respawns are applied in ascending
// entity-id order. Reuses `scratch` across ticks (no per-tick heap alloc).
void update_respawns(scene::World& w, f32 dt_seconds, std::vector<Entity>& scratch);

}  // namespace psynder::gameplay
