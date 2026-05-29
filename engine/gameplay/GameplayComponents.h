// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/GameplayComponents.h
//
// FPS gameplay ECS components (health / armor / death / respawn). POD, on the
// authoritative deterministic tick — no RNG, no per-frame alloc in the systems
// (see Damage.cpp). The home for the Quake/Battlefield-class gameplay state that
// the netcode replicates and the weapons systems mutate.

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "math/Math.h"
#include "core/Types.h"

namespace psynder::gameplay {

// Hit points. Real units: points (e.g. 0..100). Dead when hp <= 0.
PSYNDER_COMPONENT(Health) {
    f32 hp;
    f32 max_hp;
};
static_assert(sizeof(Health) == 8, "Health layout frozen");

// Armor points; absorbs a fraction of incoming damage before health hits
// (Quake-style). Empty (points == 0) => damage goes straight to health.
PSYNDER_COMPONENT(Armor) {
    f32 points;
};
static_assert(sizeof(Armor) == 4, "Armor layout frozen");

// Tag added on death; counts down respawn_in_s, then the entity respawns.
PSYNDER_COMPONENT(Dead) {
    f32 respawn_in_s;
};
static_assert(sizeof(Dead) == 4, "Dead layout frozen");

// Respawn config on a respawnable actor: where it returns + the delay.
PSYNDER_COMPONENT(Respawnable) {
    psynder::math::Vec3 spawn_pos;
    f32                 delay_s;
};
static_assert(sizeof(Respawnable) == 16, "Respawnable layout frozen");

}  // namespace psynder::gameplay
