// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — adaptive send-rate congestion control. Lane 18 (net).
//
// See CongestionControl.h for the contract and the Gaffer good/bad-mode
// rationale. Pure algebra (only a std::isfinite guard); no heap, no RNG, no
// transcendentals — bit-reproducible across platforms in the strict-FP net
// lane.

#include "net/CongestionControl.h"

#include "core/Types.h"

#include <cmath>

namespace psynder::net {

void congestion_init(CongestionState& s) noexcept {
    s.mode         = static_cast<u32>(NetMode::Good);
    s.good_timer_s = 0.0f;
    s.bad_dwell_s  = 0.0f;
}

void congestion_update(CongestionState& s, const CongestionConfig& cfg,
                       f32 rtt_s, f32 dt_s) noexcept {
    // Guard the clock: a NaN/Inf or non-positive step is meaningless and would
    // poison the timers, so skip the tick entirely and leave state untouched.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) {
        return;
    }

    // The link is "healthy" iff the smoothed RTT is at or below the threshold.
    // (A non-finite RTT compares false here, so it counts as unhealthy — the
    // safe direction.)
    const bool healthy = (rtt_s <= cfg.rtt_bad_threshold_s);

    if (s.mode == static_cast<u32>(NetMode::Good)) {
        if (!healthy) {
            // Over-react to congestion: drop to BAD on the first bad reading and
            // open a fresh dwell + recovery window.
            s.mode         = static_cast<u32>(NetMode::Bad);
            s.bad_dwell_s  = 0.0f;
            s.good_timer_s = 0.0f;
        }
        // Healthy in GOOD: nothing to do — timers stay at 0.
        return;
    }

    // BAD mode. Always rack up the time spent down here.
    s.bad_dwell_s += dt_s;

    if (healthy) {
        // Contiguous healthy time toward promotion.
        s.good_timer_s += dt_s;
    } else {
        // Any bad reading restarts the contiguous-good requirement.
        s.good_timer_s = 0.0f;
    }

    // Promote BAD -> GOOD only when BOTH hysteresis gates are satisfied: a
    // sustained contiguous-good window AND a minimum total dwell in BAD.
    if (s.good_timer_s >= cfg.promote_after_good_s &&
        s.bad_dwell_s >= cfg.min_bad_dwell_s) {
        s.mode         = static_cast<u32>(NetMode::Good);
        s.good_timer_s = 0.0f;
        s.bad_dwell_s  = 0.0f;
    }
}

f32 congestion_send_hz(const CongestionState& s,
                       const CongestionConfig& cfg) noexcept {
    return (s.mode == static_cast<u32>(NetMode::Good)) ? cfg.good_send_hz
                                                       : cfg.bad_send_hz;
}

NetMode congestion_mode(const CongestionState& s) noexcept {
    return (s.mode == static_cast<u32>(NetMode::Good)) ? NetMode::Good
                                                       : NetMode::Bad;
}

}  // namespace psynder::net
