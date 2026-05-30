// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Grenade.h — a thrown, fuse-timed grenade that detonates into
// a radial splash blast.
//
// This is the projectile-shaped weapon that composes Splash into something a
// player throws: a grenade entity carries a velocity, a countdown fuse, and the
// SplashParams to detonate with. Each authoritative lockstep tick the grenade
// arcs under gravity (the Weapons projectile integrator + a constant downward
// acceleration), and when its fuse reaches zero it calls apply_splash_damage at
// its world position and despawns. The owner is credited the kills and supplies
// the friendly-team filter, exactly as the splash system expects.
//
// On the authoritative tick: no RNG, no transcendentals (one sqrt for the dir
// normalize only), stable entity-id processing order, and no per-tick heap
// growth beyond the reused scratch vector. Same world + inputs => identical
// detonations + damage on every platform.
//
// 1 world unit = 1 metre; velocity in m/s, fuse in seconds, gravity in m/s².

#pragma once

#include "gameplay/Splash.h"  // SplashParams, apply_splash_damage

#include "scene/World.h"

#include "math/Math.h"
#include "core/Types.h"

#include <vector>

namespace psynder::gameplay {

// Real Earth surface gravity (metres per second squared), per the project's
// metric-units / real-physics pillar. Applied to a grenade's vertical velocity
// each tick so the throw follows a true ballistic arc.
inline constexpr f32 kGrenadeGravity = 9.81f;

// An in-flight grenade. Modelled on Projectile (a moving damage carrier), but
// instead of impacting the first body it reaches it arcs under gravity until its
// fuse expires, then detonates into a radial blast.
//
//   velocity   — m/s, world space (gravity pulls -Y each tick).
//   fuse_s     — seconds remaining; counts DOWN, detonates at <= 0.
//   blast      — the SplashParams applied at the detonation point.
//   owner      — credited the kills (and is itself hurt by its own blast in
//                range — the grenade-jump, matching apply_splash_damage).
//   owner_team — friendly filter passed to apply_splash_damage: when >= 0,
//                teammates carrying that Team are spared; kNoTeam (-1) is FFA.
PSYNDER_COMPONENT(Grenade) {
    psynder::math::Vec3 velocity;    // m/s, world space
    f32                 fuse_s;      // seconds to detonation (counts down)
    SplashParams        blast;       // radial blast shape applied on detonation
    Entity              owner;       // credited the kills / friendly-fire source
    i64                 owner_team;  // friendly filter (kNoTeam = free-for-all)
};
// 48 bytes: velocity(12) + fuse_s(4) = 16, blast(12) = 28, then the u64 Entity
// forces 8-byte alignment so `owner` lands at offset 32 (4 pad bytes), and the
// i64 owner_team follows at 40 — 48 total.
static_assert(sizeof(Grenade) == 48, "Grenade layout frozen");

// Throw a grenade from `thrower` at `origin`, flying along `dir` at `speed_mps`.
// The grenade gets a TransformWS at `origin`, a velocity of normalize(dir) *
// speed_mps, the given fuse + blast, and owner/owner_team for the detonation.
// Returns the new grenade entity, or an invalid Entity if `dir` is zero-length
// (no throw direction). One sqrt for the normalize; no RNG. The thrower need not
// own a Weapon — a grenade is its own self-contained munition.
Entity throw_grenade(scene::World& w, Entity thrower, i64 thrower_team,
                     math::Vec3 origin, math::Vec3 dir, f32 speed_mps,
                     f32 fuse_s, const SplashParams& blast);

// Advance every Grenade: apply gravity to velocity.y, integrate the position in
// its TransformWS, and decrement the fuse. A grenade whose fuse reaches <= 0
// DETONATES — apply_splash_damage(w, grenade_pos, blast, owner, owner_team, …)
// — and is then despawned. Deterministic: positions/fuses are integrated in
// place, detonations are GATHERED (with their resolved blast point) and then
// applied + despawned in ascending entity-id order, so iteration / spawn order
// can never change the result. Reuses `scratch` for the despawn list (no
// per-tick heap growth).
void tick_grenades(scene::World& w, f32 dt_seconds, std::vector<Entity>& scratch);

}  // namespace psynder::gameplay
