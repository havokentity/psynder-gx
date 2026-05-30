// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/RoundTimer.cpp — see RoundTimer.h. Pure time-driven round phase
// machine: Warmup -> Active -> (Overtime if configured + unresolved) -> Ended.
// Overshoot remainder carries across each transition so a single big dt lands
// correctly (and may cross multiple boundaries). Strict FP, no RNG, no alloc;
// deterministic.

#include "match/RoundTimer.h"

#include <cmath>

namespace psynder::match {

namespace {

// The duration (seconds) of the phase the timer is currently in. Ended has no
// duration; it is reported as 0 so callers see no time remaining.
f32 phase_duration(const RoundTimer& t, const RoundConfig& cfg) noexcept {
    switch (static_cast<RoundPhase>(t.phase)) {
        case RoundPhase::Warmup:   return cfg.warmup_s;
        case RoundPhase::Active:   return cfg.round_s;
        case RoundPhase::Overtime: return cfg.overtime_s;
        case RoundPhase::Ended:    return 0.0f;
    }
    return 0.0f;
}

// The phase that follows `from` when it times out, given the config and the
// sudden-death flag. Active branches to Overtime only when overtime is
// configured AND the round is still unresolved; otherwise it Ends. Ended is its
// own successor (terminal).
RoundPhase next_phase(RoundPhase from, const RoundConfig& cfg,
                      bool sudden_death_unresolved) noexcept {
    switch (from) {
        case RoundPhase::Warmup:
            return RoundPhase::Active;
        case RoundPhase::Active:
            return (cfg.overtime_s > 0.0f && sudden_death_unresolved)
                       ? RoundPhase::Overtime
                       : RoundPhase::Ended;
        case RoundPhase::Overtime:
            return RoundPhase::Ended;
        case RoundPhase::Ended:
            return RoundPhase::Ended;
    }
    return RoundPhase::Ended;
}

}  // namespace

void round_init(RoundTimer& t) noexcept {
    t.phase = static_cast<u32>(RoundPhase::Warmup);
    t.elapsed_s = 0.0f;
}

void round_tick(RoundTimer& t, const RoundConfig& cfg,
                bool sudden_death_unresolved, f32 dt_s) noexcept {
    // Guard: only ever advance by a real, positive amount. A non-finite or
    // non-positive dt is a no-op, keeping the clock monotonic + deterministic.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) return;

    // Already over: nothing to advance.
    if (static_cast<RoundPhase>(t.phase) == RoundPhase::Ended) return;

    t.elapsed_s += dt_s;

    // Resolve as many phase boundaries as this dt crosses, carrying the
    // overshoot remainder into each successive phase.
    for (;;) {
        const RoundPhase cur = static_cast<RoundPhase>(t.phase);
        if (cur == RoundPhase::Ended) {
            // Terminal: no duration to carry into; park the clock.
            t.elapsed_s = 0.0f;
            return;
        }

        const f32 dur = phase_duration(t, cfg);
        if (t.elapsed_s < dur) return;  // still within the current phase

        // Phase timed out: carry the overshoot into the next phase.
        t.elapsed_s -= dur;
        t.phase = static_cast<u32>(next_phase(cur, cfg, sudden_death_unresolved));
    }
}

f32 phase_remaining_s(const RoundTimer& t, const RoundConfig& cfg) noexcept {
    if (static_cast<RoundPhase>(t.phase) == RoundPhase::Ended) return 0.0f;
    const f32 rem = phase_duration(t, cfg) - t.elapsed_s;
    return rem > 0.0f ? rem : 0.0f;
}

RoundPhase round_phase(const RoundTimer& t) noexcept {
    return static_cast<RoundPhase>(t.phase);
}

bool round_ended(const RoundTimer& t) noexcept {
    return static_cast<RoundPhase>(t.phase) == RoundPhase::Ended;
}

}  // namespace psynder::match
