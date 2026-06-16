// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Pickups.h — deterministic floor pickups (health / ammo / weapon).

#pragma once

#include "gameplay/GameplayComponents.h"  // PickupKind

#include "scene/World.h"

#include "math/Math.h"
#include "core/Types.h"

namespace psynder::gameplay {

// Spawn an active pickup entity at `pos`.
Entity spawn_pickup(scene::World& w, PickupKind kind, f32 amount,
                    math::Vec3 pos, f32 radius_m, f32 respawn_delay_s);

// Advance pickups by dt: inactive ones count their respawn timer down; an active
// pickup grants its effect to the first overlapping player (Health entity,
// lowest id) within radius — Health heals (capped at max_hp), Ammo/Weapon add
// rounds to the player's Weapon — then goes inactive for respawn_delay_s.
// Deterministic: players considered in ascending id order; each grant is
// independent + additive.
void tick_pickups(scene::World& w, f32 dt_seconds);

}  // namespace psynder::gameplay
