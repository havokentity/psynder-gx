// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/TeamScore.cpp — see TeamScore.h. Deterministic TDM scoring:
// integer sums over the Score/Team archetype in chunk order, lowest-team-index
// tie breaks, no RNG, no per-call heap beyond the caller's reused out vector.

#include "match/TeamScore.h"

namespace psynder::match {

using gameplay::Score;
using gameplay::Team;

namespace {

// Sum a single field (frags or deaths) over all Score+Team entities whose team
// matches. for_each_chunk<Score, Team> visits only chunks holding BOTH
// components, so a Score-without-Team entity is never seen (correctly excluded).
template <class Pick>
u32 sum_team_field(scene::World& w, u32 team, Pick pick) noexcept {
    u32 total = 0;
    w.for_each_chunk<Score, Team>([&](usize n, Score* sc, Team* tm) {
        for (usize i = 0; i < n; ++i) {
            if (tm[i].team == team) total += pick(sc[i]);
        }
    });
    return total;
}

}  // namespace

u32 team_frags(scene::World& w, u32 team) noexcept {
    return sum_team_field(w, team, [](const Score& s) { return s.frags; });
}

u32 team_deaths(scene::World& w, u32 team) noexcept {
    return sum_team_field(w, team, [](const Score& s) { return s.deaths; });
}

void team_frag_totals(scene::World& w, u32 num_teams,
                      std::vector<u32>& out_frags) noexcept {
    out_frags.clear();
    out_frags.resize(num_teams, 0u);
    if (num_teams == 0u) return;
    // Single pass: bucket each player's frags into its team (in-range only).
    w.for_each_chunk<Score, Team>([&](usize n, Score* sc, Team* tm) {
        for (usize i = 0; i < n; ++i) {
            const u32 t = tm[i].team;
            if (t < num_teams) out_frags[t] += sc[i].frags;
        }
    });
}

u32 leading_team(scene::World& w, u32 num_teams) noexcept {
    if (num_teams == 0u) return 0u;
    std::vector<u32> totals;
    team_frag_totals(w, num_teams, totals);
    u32 best_team = 0u;
    u32 best_frags = totals[0];
    // Strictly greater => the first (lowest-index) team wins exact ties.
    for (u32 t = 1u; t < num_teams; ++t) {
        if (totals[t] > best_frags) {
            best_frags = totals[t];
            best_team = t;
        }
    }
    return best_team;
}

bool team_frag_limit_reached(scene::World& w, u32 num_teams, u32 frag_limit,
                             u32& out_winner) noexcept {
    if (num_teams == 0u) return false;
    std::vector<u32> totals;
    team_frag_totals(w, num_teams, totals);
    u32 best_team = 0u;
    u32 best_frags = totals[0];
    for (u32 t = 1u; t < num_teams; ++t) {
        if (totals[t] > best_frags) {
            best_frags = totals[t];
            best_team = t;
        }
    }
    if (best_frags >= frag_limit) {
        out_winner = best_team;  // highest frags, ties -> lowest index
        return true;
    }
    return false;
}

}  // namespace psynder::match
