// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Suppression.h — being-shot-at degrades accuracy.
//
// A Suppression is a per-pawn "keep their heads down" resource that sits
// ALONGSIDE the pawn's other gameplay state (additive). Near-misses and incoming
// fire raise `level` (0..1); it decays back toward 0 over time. While the level
// is high a future fire path widens weapon spread / lowers accuracy by the value
// `spread_multiplier()` returns. The raise / decay are pure algebra on the
// authoritative lockstep tick: no RNG, no per-entity cross state, so the same
// inputs produce identical state (determinism is a hard pillar — see Stamina.cpp
// / Damage.cpp).
//
// Driving model: the weapon / hit-detection layer calls add_suppression() on a
// pawn whose vicinity a round passes through (a near-miss) or that takes fire.
// Each tick the caller invokes tick_suppressions(world, dt) to decay every pawn's
// suppression (the raise events are sparse; the decay is uniform across all). A
// shooter then multiplies its base spread by spread_multiplier(shooter.Suppression)
// when it fires.

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "core/Types.h"

namespace psynder::gameplay {

// Suppression state. `level` is the current suppression in [0, 1] (0 = calm,
// 1 = fully suppressed). It decays toward 0 at `decay_per_s` per second.
// `max_spread_mult` (>= 1) is the weapon-spread multiplier at FULL suppression;
// at level 0 the multiplier is 1.0 (no penalty), lerping linearly between.
PSYNDER_COMPONENT(Suppression) {
    f32 level;            // current suppression, clamped to [0, 1]
    f32 decay_per_s;      // how fast level falls toward 0 each second
    f32 max_spread_mult;  // spread multiplier at level 1 (>= 1; 1 = no penalty)
};
static_assert(sizeof(Suppression) == 12, "Suppression layout frozen");

// Raise suppression by `amount` (a near-miss / incoming-fire event):
//   level = clamp(level + amount, 0, 1).
// Negative amounts are permitted (they lower level) but are still clamped.
void add_suppression(Suppression& s, f32 amount) noexcept;

// Decay a single pawn's suppression by dt_s:
//   level = clamp(level - decay_per_s*dt, 0, 1).
// Non-finite or non-positive dt is ignored (no-op), so a stalled / bad frame
// cannot push level out of range. Deterministic.
void tick_suppression(Suppression& s, f32 dt_s) noexcept;

// The weapon-spread multiplier for this suppression, in [1, max_spread_mult]:
// lerp from 1.0 at level 0 to max_spread_mult at level 1, linearly by level.
// A suppressed shooter is LESS accurate, so callers multiply their base spread
// (cone half-angle) by this value before sampling a shot direction.
f32 spread_multiplier(const Suppression& s) noexcept;

// True iff level >= threshold — a gate for "is this pawn pinned" gameplay /
// AI logic (e.g. a bot ducks for cover while suppressed).
bool is_suppressed(const Suppression& s, f32 threshold) noexcept;

// Decay EVERY Suppression in the world by dt_s (tick_suppression per entity).
// The raise events (add_suppression) are sparse and driven by the fire path;
// this is the uniform per-tick decay over all pawns. Order-independent: each
// entity's decay is a pure per-entity update (no cross-entity state, no
// structural changes), so chunk/storage order does not affect the result.
void tick_suppressions(scene::World& w, f32 dt_s) noexcept;

}  // namespace psynder::gameplay
