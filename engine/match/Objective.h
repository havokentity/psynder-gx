// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/Objective.h — deterministic capture-point (control-point)
// objective for Domination / King-of-the-Hill modes: the objective-mode analog
// of gameplay::MatchRules' phase machine. A ControlPoint is a tiny struct driven
// by a pure state machine over per-team presence counts the caller supplies (how
// many of each team currently stand on the point) — no ECS dependency.
//
// Capture rule: when EXACTLY one team occupies the point and it is not already
// the owner, capture progress climbs toward that team at capture_rate; at full
// progress the point flips to that team. A contested point (two-or-more teams
// present) or an empty point — and likewise the owner standing on its own point
// — stalls and DECAYS any partial progress back toward zero at decay_rate.
//
// On the authoritative lockstep tick: pure algebra, strict FP, no RNG, no alloc.
// Same inputs => bit-identical state on every platform. (Mirrors the
// MatchRules.h tick_match / deterministic-tie-break idiom for the match lane.)

#pragma once

#include "core/Types.h"

#include <span>

namespace psynder::match {

// Sentinel team index: neutral / unowned point, or "no team is capturing".
inline constexpr u32 kNoTeam = 0xFFFFFFFFu;

// A single capturable objective. Caller-owned (one per control point in a map);
// not an ECS component. `owner_team` holds the point (kNoTeam = neutral).
// `capturing_team` is the team currently making progress (kNoTeam = none).
// `progress` is in [0,1] toward `capturing_team`'s capture.
struct ControlPoint {
    u32 owner_team;      // who holds the point (kNoTeam = neutral)
    u32 capturing_team;  // team currently capturing (kNoTeam = none)
    f32 progress;        // [0,1] toward capturing_team's capture
};

// Reset to a neutral point: no owner, no capturer, zero progress.
void control_point_init(ControlPoint& cp) noexcept;

// The single team index that is the ONLY team present on the point. `team_counts`
// is indexed by team; team t is "present" iff team_counts[t] != 0. Returns that
// team index iff exactly one team is present; returns kNoTeam when zero teams are
// present (empty) or two-or-more are present (contested).
u32 sole_occupant(std::span<const u32> team_counts) noexcept;

// True iff two-or-more teams are present on the point (a contested point, which
// never makes capture progress).
bool is_contested(std::span<const u32> team_counts) noexcept;

// Advance the control point by `dt_s`. Let sole = sole_occupant(team_counts):
//   * If sole is a real team AND sole != owner_team (someone new is capturing):
//     capturing_team = sole; progress += capture_rate_per_s * dt_s. When progress
//     reaches 1 the point FLIPS — owner_team = sole, progress = 0, capturing_team
//     = kNoTeam (capture complete; the new owner is no longer "capturing").
//   * Otherwise (contested, empty, OR the sole occupant already owns the point):
//     progress DECAYS toward 0 by decay_rate_per_s * dt_s (clamped at 0); when it
//     reaches 0, capturing_team is cleared to kNoTeam.
// progress is always clamped to [0,1]. Pure + deterministic: no RNG, no alloc.
void tick_control_point(ControlPoint& cp, std::span<const u32> team_counts,
                        f32 capture_rate_per_s, f32 decay_rate_per_s,
                        f32 dt_s) noexcept;

}  // namespace psynder::match
