// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/StatusEffect.h — timed damage-over-time status effects
// (Burn / Poison / Bleed) an entity carries.
//
// A StatusEffects component holds, per kind, a countdown timer, a damage rate
// (damage/sec) and the raw id of the attacker who applied it. tick_status runs
// each authoritative frame: it drains the timers and bleeds dps*dt damage into
// the victim's Health, crediting the kill to whoever applied the effect.
//
// Deterministic: pure algebra over the timers, stable processing order
// (ascending entity-id, then enum order), no RNG, no per-tick heap alloc beyond
// a reused gather scratch. Mirrors the POD/system split of Damage.{h,cpp} and
// Powerup.{h,cpp}.

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "core/Types.h"

namespace psynder::gameplay {

// The damage-over-time kinds. Stored as the index into the StatusEffects
// per-kind arrays; values are stable so they double as wire/save ids.
enum class StatusKind : u32 {
    Burn = 0,    // fire DoT
    Poison = 1,  // toxin DoT
    Bleed = 2,   // wound DoT
};

// Number of distinct status kinds (size of each per-entity array).
inline constexpr u32 kStatusKindCount = 3;

// Per-entity damage-over-time state, one slot per StatusKind (indexed by its
// enum value): seconds remaining, damage/sec, and the raw id of the attacker
// that applied that kind. POD; replicated by the netcode and mutated by the
// status systems. A kind is active iff time_left[kind] > 0.
PSYNDER_COMPONENT(StatusEffects) {
    f32 time_left[kStatusKindCount];  // seconds remaining per kind; 0 = inactive
    f32 dps[kStatusKindCount];        // damage/sec per kind while active
    u64 source[kStatusKindCount];     // attacker Entity::raw per kind (0 = none)
};
static_assert(sizeof(StatusEffects) ==
                  sizeof(f32) * kStatusKindCount * 2 + sizeof(u64) * kStatusKindCount,
              "StatusEffects layout frozen (time_left + dps + source per kind)");
static_assert(sizeof(StatusEffects) == 48,
              "StatusEffects is 3 f32 timers + 3 f32 dps + 3 u64 sources == 48 bytes");

// Apply (or refresh) status `k` on `s` for `duration_s` seconds at `dps`
// damage/sec, attributed to `source`.
//
// Refresh-not-stack rule: re-applying does NOT add a second instance. The
// timer is refreshed to max(remaining, duration_s) — re-applying never shortens
// an active effect — and the rate is raised to max(existing dps, dps) so a
// stronger application wins while a weaker one cannot weaken an active effect.
// The most recent application records the source (so the latest attacker is
// credited for the kill). Deterministic; clamps a negative duration to 0.
void apply_status(StatusEffects& s, StatusKind k, f32 duration_s, f32 dps,
                  Entity source) noexcept;

// True iff status `k` is currently active (time_left[k] > 0).
bool has_status(const StatusEffects& s, StatusKind k) noexcept;

// Advance every StatusEffects entity (it must also have a Health) by
// `dt_seconds`: for each active kind decrement its timer by dt and bleed
// dps*dt damage into the victim this tick, crediting the recorded source via
// damage_credited (so a DoT kill is attributed to whoever applied the effect);
// if the source is invalid or the victim itself, plain apply_damage is used.
// When a kind's timer reaches 0 it clears (rate + source reset). A DEAD entity
// has all its effects cleared and takes no DoT (no damage to a corpse).
//
// Damage is applied CONTINUOUSLY as dps*dt each tick, not in 1-second chunks,
// so the total dealt over the duration is ~= dps*duration_s (exact up to the
// final partial tick).
//
// Deterministic: timers tick in place during a read-only walk, then the
// gathered victims are processed in ascending entity-id order with kinds in
// enum order, so the result is independent of chunk/storage order. No RNG; the
// gather scratch is reused across ticks (no per-tick heap alloc).
void tick_status(scene::World& w, f32 dt_seconds);

}  // namespace psynder::gameplay
