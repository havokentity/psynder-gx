// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Weapons.h — deterministic hitscan + projectile weapons.
//
// On the authoritative lockstep tick. fire_hitscan / fire_projectile gate on the
// shooter's Weapon (fire rate + ammo); damage is applied via the gameplay Damage
// system. The server lag-comp rewind (the net ReplicationSession already rewinds
// to the shooter's view-tick + produces HitEvents) feeds the SAME apply_damage —
// wiring net HitEvents to these is the client/server-split integration follow-up.

#pragma once

#include "scene/World.h"

#include "math/Math.h"
#include "core/Types.h"

#include <vector>

namespace psynder::gameplay {

inline constexpr f32 kProjectileHitRadius = 0.5f;  // proximity damage radius (m)

// No-team sentinel for fire_hitscan's friendly_team parameter (free-for-all).
inline constexpr i64 kNoTeam = -1;

// Hitscan fire from `shooter` along `dir` from `origin`. If its Weapon is ready
// (cooldown <= 0 and ammo != 0): spend a round + set the cooldown, ray-test the
// nearest Health entity (within the weapon's hit_radius of the ray, excluding
// the shooter), and apply the weapon damage to it. Returns the victim (an
// invalid Entity on miss or if not ready). Deterministic (nearest by hit
// distance, ties broken by lower entity id).
//
// `friendly_team` enables team-aware friendly fire: when >= 0, any candidate
// carrying a Team component equal to it is skipped (a teammate in the line of
// fire is shot through, not hit). kNoTeam (-1, the default) keeps the classic
// free-for-all behaviour, so existing callers are unaffected.
Entity fire_hitscan(scene::World& w, Entity shooter, math::Vec3 origin,
                    math::Vec3 dir, i64 friendly_team = kNoTeam) noexcept;

// Spawn a projectile from `shooter`'s Weapon (must be ready; spends a round +
// sets the cooldown). The projectile flies at `speed_mps` along `dir`, carries
// the weapon's damage, and lives `ttl_s` seconds. Returns the projectile entity
// (invalid if not ready).
Entity fire_projectile(scene::World& w, Entity shooter, math::Vec3 origin,
                       math::Vec3 dir, f32 speed_mps, f32 ttl_s);

// Advance every Weapon cooldown by dt (order-independent, deterministic).
void tick_weapons(scene::World& w, f32 dt_seconds);

// Advance projectiles: integrate by velocity; on reaching a Health entity (not
// the owner, within kProjectileHitRadius) apply damage + despawn; despawn on ttl
// expiry. Deterministic: integration is in place; hits/despawns are applied in
// ascending entity-id order. Reuses `scratch` for the despawn list.
void tick_projectiles(scene::World& w, f32 dt_seconds, std::vector<Entity>& scratch);

}  // namespace psynder::gameplay
