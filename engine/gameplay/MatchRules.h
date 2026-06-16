// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/MatchRules.h — deterministic match/round orchestration for the
// Quake3-arena loop: a phase machine (Warmup -> Active -> Intermission ->
// Warmup), frag-limit / time-limit win conditions, and deterministic
// spawn-point selection (farthest-from-enemy, the classic anti-spawn-camp
// heuristic). On the authoritative lockstep tick: no RNG, stable id-ordered tie
// breaks, allocation-free scans. Ties the existing Score / Health / TransformWS
// gameplay components into an actual match.

#pragma once

#include "scene/World.h"

#include "math/Math.h"
#include "core/Types.h"

#include <span>

namespace psynder::gameplay {

enum class MatchPhase : u32 {
    Warmup = 0,        // pre-match: clock not counting toward win conditions
    Active = 1,        // live: scoring + win-condition checks
    Intermission = 2,  // post-match: scoreboard hold, then back to Warmup
};

// Match rules (Quake3-style: first to frag_limit, or highest score at
// time_limit). A limit of 0 disables that condition; with both disabled the
// match runs until stop_match() is called externally.
struct MatchConfig {
    u32 frag_limit = 0;          // first player to this many frags wins (0 = off)
    f32 time_limit_s = 0.0f;     // match ends at this elapsed Active time (0 = off)
    f32 warmup_s = 0.0f;         // Warmup duration before Active
    f32 intermission_s = 5.0f;   // Intermission (scoreboard) duration
};

// Live match state. One per match/session (not an ECS component).
struct MatchState {
    MatchPhase phase = MatchPhase::Warmup;
    f32        time_in_phase_s = 0.0f;  // elapsed within the current phase
    f32        match_time_s = 0.0f;     // elapsed within Active (resets each match)
    Entity     winner{};                // set when entering Intermission (invalid = draw)
    bool       just_ended = false;      // true only on the Active->Intermission tick
    u32        round = 0;               // increments each time a new match goes Active
};

inline bool match_live(const MatchState& m) noexcept {
    return m.phase == MatchPhase::Active;
}

// Highest-frags entity (ties -> lowest entity id, deterministic). Returns an
// invalid Entity{} if there are no Score entities; writes the leader's frag
// count to `out_frags` when non-null.
Entity frag_leader(scene::World& w, u32* out_frags = nullptr) noexcept;

// Advance the match clock + phase machine by dt. In Active it checks the win
// conditions (frag_leader vs frag_limit; match_time vs time_limit) and, on a
// win, records the winner and transitions to Intermission (sets just_ended).
// Intermission elapses back to Warmup; Warmup elapses to Active (round++).
void tick_match(MatchState& m, scene::World& w, const MatchConfig& cfg,
                f32 dt) noexcept;

// Deterministic spawn-point selection: the spawn whose nearest LIVING player
// (Health.hp > 0, excluding `self`) is farthest away — the classic Quake spawn
// heuristic that avoids dropping a player next to an enemy. Ties -> lowest
// index. Allocation-free (rescans players per candidate; counts are small).
// Returns 0 when `spawns` is empty or there are no other living players.
usize select_spawn(scene::World& w, std::span<const math::Vec3> spawns,
                   Entity self) noexcept;

}  // namespace psynder::gameplay
