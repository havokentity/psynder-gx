// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/TargetSelect.h
//
// Deterministic best-target selection for a combatant choosing whom to engage.
// Given a chooser's world position + team and a flat list of candidate targets,
// pick the best ENEMY (different team) by a composite score that rewards nearer,
// visible, and lower-health targets — so a bot finishes wounded, exposed enemies
// rather than chasing a healthy one across the map.
//
// The ai lane links only core + math, so this is a pure function over plain
// structs (no ECS / gameplay dependency); the caller fills TargetCandidate from
// whatever its own components are.
//
// Determinism (lockstep pillar): pure algebra plus exactly one sqrt per distance
// (no RNG, no trig). Ties on the composite score break to the LOWER candidate id,
// mirroring FlowField's lowest-index tie-break, so the same inputs always yield
// the same pick on every platform. Build -fno-fast-math.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <span>

namespace psynder::ai {

// One potential target as seen by the chooser. `team` is compared for inequality
// only (any different value is an enemy); `health` is in the same 0..100 scale the
// scoring assumes; `visible` is the caller's line-of-sight result.
struct TargetCandidate {
    u32        id;
    math::Vec3 pos;
    u32        team;
    f32        health;
    bool       visible;
};

// How much each factor pulls on the composite score, plus the hard engagement
// cutoff. Larger weights == that factor matters more.
struct TargetWeights {
    f32 distance_weight;   // penalty per metre of distance (subtracted)
    f32 visible_bonus;     // flat reward when the target is visible
    f32 low_health_bonus;  // reward scaling from 0 (full health) to this (dead)
    f32 max_range_m;       // candidates farther than this are ignored entirely
};

// Sensible defaults: visibility dominates a few tens of metres of distance, a
// near-dead enemy is worth ~one extra "visible" pull, and we won't engage past
// 80 m. Tuned so a visible target almost always beats an equidistant hidden one.
inline constexpr TargetWeights kDefaultTargetWeights{
    /*distance_weight=*/1.0f,
    /*visible_bonus=*/50.0f,
    /*low_health_bonus=*/40.0f,
    /*max_range_m=*/80.0f,
};

// Composite desirability of a single target; higher == better to engage.
//
//   d        = length(cand_pos - chooser_pos)              (metres, one sqrt)
//   hfrac    = clamp(cand_health / 100, 0, 1)              (1 = full, 0 = dead)
//   score    = visible_bonus * (visible ? 1 : 0)
//            - distance_weight * d
//            + low_health_bonus * (1 - hfrac)
//
// Nearer => smaller distance penalty => higher score. Visible adds a flat bonus.
// Lower health => larger (1 - hfrac) => higher score (finish the wounded).
// Range gating is the selector's job; this scores any target it is handed.
f32 target_score(math::Vec3 chooser_pos, math::Vec3 cand_pos, f32 cand_health,
                 bool visible, const TargetWeights& w) noexcept;

// Among `candidates` on a DIFFERENT team than `chooser_team` and within
// `w.max_range_m` of `chooser_pos`, pick the one with the highest target_score.
// Ties on the score break to the LOWER id (deterministic). On success writes the
// winner's id to `out_id` and returns true; if no candidate is eligible returns
// false and leaves `out_id` untouched.
bool select_target(math::Vec3 chooser_pos, u32 chooser_team,
                   std::span<const TargetCandidate> candidates,
                   const TargetWeights& w, u32& out_id) noexcept;

}  // namespace psynder::ai
