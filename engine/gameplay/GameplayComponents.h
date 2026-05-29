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

// A weapon held by a shooter. fire_interval_s gates the fire rate; cooldown_s
// counts down to 0 (ready). ammo < 0 means infinite. hit_radius is the target
// hitbox sphere the hitscan ray tests against (metres).
PSYNDER_COMPONENT(Weapon) {
    f32 damage;
    f32 fire_interval_s;
    f32 cooldown_s;
    i32 ammo;
    f32 hit_radius;
};
static_assert(sizeof(Weapon) == 20, "Weapon layout frozen");

// A flying projectile (rocket / grenade-ish): integrates by velocity, damages
// the first Health entity it reaches, and despawns on hit or ttl. `owner` is
// excluded from its own hits.
PSYNDER_COMPONENT(Projectile) {
    psynder::math::Vec3 velocity;  // m/s, world space
    f32                 damage;
    f32                 ttl_s;
    Entity              owner;
};
// 32 bytes: the u64 Entity forces 8-byte alignment (20 data bytes -> padded).
static_assert(sizeof(Projectile) == 32, "Projectile layout frozen");

}  // namespace psynder::gameplay
