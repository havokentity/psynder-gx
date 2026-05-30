// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Stamina.h — sprint stamina resource.
//
// A Stamina is the sprint resource that sits ALONGSIDE the player's other
// gameplay state (additive). It drains while the pawn is sprinting and, after a
// short delay once sprinting stops, regenerates back toward full. The drain /
// regen are pure algebra on the authoritative lockstep tick: no RNG, no
// per-entity cross state, so the same inputs produce identical state
// (determinism is a hard pillar — see Damage.cpp / Reload.cpp).
//
// Driving model: the input layer knows which pawn is the controlled one. Each
// tick it calls tick_stamina(controlled.Stamina, sprinting, dt) for that pawn
// (passing whether sprint is held), and tick_staminas(world, dt) to regenerate
// every OTHER Stamina (which are treated as not-sprinting). A bot that sprints
// likewise drives its own pawn through tick_stamina.

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "core/Types.h"

namespace psynder::gameplay {

// Sprint resource. `current` is the stamina remaining, always kept in
// [0, max_stamina]. While sprinting it drains at `drain_per_s`; once the pawn
// stops, `regen_cooldown_s` counts down from `regen_delay_s` and, after it
// reaches 0, `current` refills at `regen_per_s` toward `max_stamina`. Sprinting
// resets `regen_cooldown_s` back to `regen_delay_s`, interrupting any regen.
PSYNDER_COMPONENT(Stamina) {
    f32 current;            // remaining, clamped to [0, max_stamina]
    f32 max_stamina;        // capacity (points / seconds-of-sprint scale)
    f32 drain_per_s;        // depletion rate while sprinting
    f32 regen_per_s;        // refill rate once the delay has elapsed
    f32 regen_delay_s;      // wait after sprinting stops before regen resumes
    f32 regen_cooldown_s;   // counts down to 0 before regen resumes
};
static_assert(sizeof(Stamina) == 24, "Stamina layout frozen");

// True iff there is stamina left to sprint on (current > 0).
bool can_sprint(const Stamina& s) noexcept;

// Advance a single pawn's stamina by dt_s.
//   - If `sprinting` AND current > 0: drain (current -= drain_per_s*dt, clamp 0)
//     and reset the regen wait (regen_cooldown_s = regen_delay_s).
//   - Otherwise: tick regen_cooldown_s down by dt; once it has reached 0, refill
//     (current += regen_per_s*dt, clamp to max_stamina).
// `current` is always clamped to [0, max_stamina]. Deterministic.
void tick_stamina(Stamina& s, bool sprinting, f32 dt_s) noexcept;

// Regenerate EVERY Stamina in the world by dt_s, treating them all as
// not-sprinting (i.e. tick_stamina(s, /*sprinting=*/false, dt_s) per entity).
// The controlled pawn's sprinting case is driven separately by the caller via
// tick_stamina(s, sprinting, dt_s); call this to regenerate all the rest.
// Order-independent: each entity's regen is a pure per-entity update (no
// cross-entity state, no structural changes), so chunk/storage order does not
// affect the result.
void tick_staminas(scene::World& w, f32 dt_s) noexcept;

// current / max_stamina in [0, 1]. Returns 0 when max_stamina <= 0 (guarded).
f32 stamina_fraction(const Stamina& s) noexcept;

}  // namespace psynder::gameplay
