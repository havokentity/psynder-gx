// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/TeamScore.h — deterministic team-deathmatch (TDM) scoring: the
// TEAM analog of gameplay::frag_leader. Aggregates the per-player Score
// component by its Team and decides TDM outcomes (leading team, frag-limit win).
//
// Pure reads over the gameplay ECS — no mutation, no RNG, no per-call heap
// beyond the caller's reused out vector. On the authoritative lockstep tick:
// strict FP, integer sums in archetype/chunk order (sum is order-independent),
// stable lowest-team-index tie breaks. Same world => identical results on every
// platform. The FFA scoreboard lives on Score/Team; this lane turns it into a
// team total. (Mirrors gameplay/MatchRules.h's frag_leader / win-condition
// idiom; the match lane LINKS psynder_gameplay so it includes the components.)

#pragma once

#include "gameplay/GameplayComponents.h"  // Score, Team
#include "scene/World.h"

#include "core/Types.h"

#include <vector>

namespace psynder::match {

// Sum Score.frags over every entity that has BOTH a Score and a Team == `team`.
// An entity with a Score but no Team is not in any team and is excluded.
// Deterministic: an integer sum, order-independent.
u32 team_frags(scene::World& w, u32 team) noexcept;

// As team_frags, but sums Score.deaths for the team.
u32 team_deaths(scene::World& w, u32 team) noexcept;

// Fill `out_frags` (sized to num_teams, clear+resize) with out_frags[t] =
// team_frags(w, t) for t in [0, num_teams). Reuses the caller's vector — a
// single pass over the Score/Team entities, no per-call heap. Teams with no
// players read 0.
void team_frag_totals(scene::World& w, u32 num_teams,
                      std::vector<u32>& out_frags) noexcept;

// The team index in [0, num_teams) with the highest total frags; ties break to
// the LOWEST team index. Returns 0 when num_teams == 0.
u32 leading_team(scene::World& w, u32 num_teams) noexcept;

// TDM win condition: true iff some team's total frags >= frag_limit, with
// out_winner set to the leading team (the leading_team rule: highest frags,
// ties -> lowest index). Returns false (out_winner untouched) otherwise.
bool team_frag_limit_reached(scene::World& w, u32 num_teams, u32 frag_limit,
                             u32& out_winner) noexcept;

}  // namespace psynder::match
