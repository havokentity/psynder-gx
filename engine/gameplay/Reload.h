// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Reload.h — weapon reload / magazine state machine.
//
// A Magazine is the ammo + reload state that sits ALONGSIDE the Weapon
// component (additive, not part of it): rounds loaded in the mag, reserve ammo,
// and a timed reload. The fire path consumes a round per shot; when the mag runs
// low the shooter starts a reload, which after reload_time_s tops the mag up
// from the reserve. Pure integer/float, no RNG: on the authoritative lockstep
// tick the same inputs produce identical state (determinism is a hard pillar).

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "core/Types.h"

namespace psynder::gameplay {

// A magazine + reserve with a timed reload. `in_mag` is the rounds loaded;
// `reserve` is the spare ammo a reload draws from; `mag_size` is the mag
// capacity. A reload takes `reload_time_s`; `reload_left_s` counts it down and,
// while > 0, the magazine is mid-reload (cannot fire). On completion the mag is
// topped up from the reserve by min(mag_size - in_mag, reserve) rounds.
PSYNDER_COMPONENT(Magazine) {
    i32 in_mag;
    i32 reserve;
    i32 mag_size;
    f32 reload_time_s;
    f32 reload_left_s;  // > 0 => mid-reload
};
static_assert(sizeof(Magazine) == 20, "Magazine layout frozen");

// True iff the magazine has a round loaded AND is not currently reloading.
bool can_fire(const Magazine& m) noexcept;

// If can_fire, spend one round (in_mag -= 1) and return true; else return false.
bool consume_round(Magazine& m) noexcept;

// Begin a reload iff one is not already running, the mag isn't full
// (in_mag < mag_size) and there is reserve to draw from (reserve > 0). Sets
// reload_left_s = reload_time_s. Returns whether a reload actually started.
bool start_reload(Magazine& m) noexcept;

// True iff a reload is in progress (reload_left_s > 0).
bool reloading(const Magazine& m) noexcept;

// Advance an in-progress reload by dt_s. On completion (reload_left_s <= 0) move
// min(mag_size - in_mag, reserve) rounds from reserve into in_mag and clamp
// reload_left_s to 0. A no-op when not reloading. Deterministic.
void tick_reload(Magazine& m, f32 dt_s) noexcept;

// Tick every Magazine in the world by dt_s. Order-independent: each entity's
// reload is a pure per-entity update (no cross-entity state, no structural
// changes), so chunk/storage order does not affect the result.
void tick_reloads(scene::World& w, f32 dt_s) noexcept;

}  // namespace psynder::gameplay
