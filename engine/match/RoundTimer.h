// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/RoundTimer.h — a deterministic, purely TIME-driven round clock
// for the match lane. Where gameplay::MatchRules ends a round on a score-based
// win condition (frag limit / leading team), RoundTimer drives a round through
// timed phases solely on elapsed time:
//
//     Warmup (countdown to start)
//       -> Active (the round body, with optional overtime)
//       -> Overtime (only if configured AND the round is still unresolved)
//       -> Ended
//
// The two are complementary: a session typically ticks both, ending the round
// on whichever fires first. RoundTimer owns the clock; MatchRules owns the
// score logic.
//
// On the authoritative lockstep tick: pure algebra, strict FP, no RNG, no
// alloc. Same (config, dt sequence, sudden_death flag) => bit-identical phase +
// elapsed on every platform. (Mirrors the Objective.h / MatchRules.h tick
// idiom for the match lane.)

#pragma once

#include "core/Types.h"

namespace psynder::match {

// Which timed phase the round is in. Stored in RoundTimer::phase as a u32.
enum class RoundPhase : u32 {
    Warmup   = 0,  // pre-round countdown; ends at warmup_s
    Active   = 1,  // the live round; ends at round_s
    Overtime = 2,  // optional sudden-death extension; ends at overtime_s
    Ended    = 3,  // terminal: the round is over, the clock is parked
};

// Durations (seconds) for each phase. overtime_s == 0 disables overtime: when
// Active times out the round goes straight to Ended regardless of the
// sudden_death flag. A non-positive warmup_s/round_s simply means that phase
// times out on the very first tick that reaches it (it carries its full dt
// remainder into the next phase).
struct RoundConfig {
    f32 warmup_s;    // Warmup countdown duration
    f32 round_s;     // Active round duration
    f32 overtime_s;  // Overtime duration (0 => no overtime)
};

// Live round clock. One per round/session (not an ECS component). `phase` holds
// a RoundPhase value; `elapsed_s` is the time accrued WITHIN the current phase
// (it resets to 0 — carrying any overshoot remainder — on each transition).
struct RoundTimer {
    u32 phase;      // RoundPhase
    f32 elapsed_s;  // elapsed within the current phase
};

// Reset to a fresh round: Warmup phase, zero elapsed.
void round_init(RoundTimer& t) noexcept;

// Advance the round clock by `dt_s`.
//
// `elapsed_s` accrues by dt; when it reaches the current phase's duration the
// phase transitions:
//   * Warmup  -> Active   at warmup_s.
//   * Active  -> Overtime at round_s, but only when overtime_s > 0 AND
//               `sudden_death_unresolved` is true; otherwise Active -> Ended.
//   * Overtime-> Ended    at overtime_s.
//   * Ended is terminal (no further advance).
//
// CARRY BEHAVIOUR: on every transition `elapsed_s` is reset to the OVERSHOOT
// remainder (elapsed - phase_duration), not to 0, so a single large dt that
// jumps a phase boundary lands in the next phase with the correct leftover time
// rather than discarding it. The carry is applied repeatedly, so one dt may
// cross multiple boundaries in a single call (e.g. a huge dt can sweep
// Warmup -> Active -> Ended at once) and still land deterministically. The
// terminal Ended phase parks elapsed at 0 (it has no duration to carry into).
//
// Non-finite or non-positive dt_s is ignored (no-op): the clock only ever moves
// forward by a real, positive amount, which keeps it deterministic and
// monotonic across pauses / frame hitches.
void round_tick(RoundTimer& t, const RoundConfig& cfg,
                bool sudden_death_unresolved, f32 dt_s) noexcept;

// Seconds left in the current phase: phase_duration - elapsed_s, clamped at 0.
// Always 0 in the terminal Ended phase (it has no duration).
f32 phase_remaining_s(const RoundTimer& t, const RoundConfig& cfg) noexcept;

// The current phase as a RoundPhase enum.
RoundPhase round_phase(const RoundTimer& t) noexcept;

// True iff the round has reached the terminal Ended phase.
bool round_ended(const RoundTimer& t) noexcept;

}  // namespace psynder::match
