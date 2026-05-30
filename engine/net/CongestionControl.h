// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — adaptive send-rate congestion control. Lane 18 (net).
//
// The classic Glenn Fiedler ("Gaffer On Games") good/bad-mode flow control: a
// two-state hysteresis machine that throttles the outbound send rate when the
// link looks congested and restores it once the link has been healthy for a
// sustained period.
//
//   GOOD mode — link is healthy; send at the high rate (good_send_hz).
//   BAD  mode — link is congested; send at the conservative rate (bad_send_hz).
//
// The health signal here is the smoothed round-trip time produced by the RTT
// estimator (RttEstimator.h, this session): an RTT at or below the configured
// threshold is "healthy", anything above is "unhealthy". A single unhealthy
// reading drops us to BAD immediately (congestion is cheap to over-react to);
// climbing back out is deliberately slow and guarded by hysteresis so the send
// rate does not flap:
//
//   * the RTT must stay healthy for a contiguous `promote_after_good_s`, AND
//   * we must have spent at least `min_bad_dwell_s` total in BAD,
//
// before BAD -> GOOD is allowed. Any unhealthy reading during recovery resets
// the contiguous-good timer to zero, so a flapping link never sneaks back up.
//
// Determinism: this is the strict-FP net lane (-fno-fast-math
// -ffp-contract=off, DESIGN-PSYNDER-GX.md §14). Pure algebra — no RNG, no
// transcendentals, no library calls beyond a finiteness guard — so the same
// (rtt_s, dt_s) sequence yields a bit-identical mode and send rate on every
// platform. No per-call heap: the whole state is three POD fields.
//
// Compile flag: net TUs that include this header MUST be compiled with
//   -fno-fast-math -ffp-contract=off
// (DESIGN-PSYNDER-GX.md §14, lockstep determinism).

#pragma once

#include "core/Types.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// NetMode — the two flow-control regimes.
// ──────────────────────────────────────────────────────────────────────────
enum class NetMode : u32 {
    Good = 0,  // link healthy — send at the high rate.
    Bad  = 1,  // link congested — send at the conservative rate.
};

// ──────────────────────────────────────────────────────────────────────────
// CongestionConfig — tuning knobs; immutable for the lifetime of a session.
// ──────────────────────────────────────────────────────────────────────────
struct CongestionConfig {
    // RTT (seconds) at or below which the link is "healthy"; strictly above
    // this is "unhealthy" and forces / holds BAD mode.
    f32 rtt_bad_threshold_s;

    // Outbound send rate (Hz) while in GOOD mode.
    f32 good_send_hz;

    // Outbound send rate (Hz) while in BAD mode.
    f32 bad_send_hz;

    // Contiguous healthy time (seconds) required before BAD -> GOOD promotion.
    f32 promote_after_good_s;

    // Minimum total time (seconds) to remain in BAD before any promotion is
    // considered — the dwell floor that stops the rate flapping.
    f32 min_bad_dwell_s;
};

// Sensible defaults: 250 ms RTT threshold, 30 Hz good / 10 Hz bad, recover only
// after a contiguous second of health and at least a second spent in BAD.
inline constexpr CongestionConfig kDefaultCongestion{
    /*rtt_bad_threshold_s =*/0.25f,
    /*good_send_hz        =*/30.0f,
    /*bad_send_hz         =*/10.0f,
    /*promote_after_good_s=*/1.0f,
    /*min_bad_dwell_s     =*/1.0f,
};

// ──────────────────────────────────────────────────────────────────────────
// CongestionState — the running machine. POD; copy/serialise freely.
// ──────────────────────────────────────────────────────────────────────────
struct CongestionState {
    u32 mode;          // current NetMode (stored as its underlying u32).
    f32 good_timer_s;  // contiguous healthy time accumulated (BAD-mode recovery).
    f32 bad_dwell_s;   // total time spent in BAD since the last demotion.
};

// Reset to GOOD mode with both timers zeroed.
void congestion_init(CongestionState& s) noexcept;

// Advance the machine by `dt_s` seconds given the latest smoothed RTT `rtt_s`.
//
// "healthy" := (rtt_s <= cfg.rtt_bad_threshold_s).
//
//   GOOD mode:
//     * unhealthy -> demote to BAD immediately; reset bad_dwell and good_timer
//                    to 0 (a fresh dwell + recovery window begins).
//     * healthy   -> stay GOOD; timers remain 0.
//   BAD mode:
//     * always accumulate bad_dwell by dt.
//     * healthy   -> accumulate good_timer by dt.
//     * unhealthy -> reset good_timer to 0 (recovery must restart contiguous).
//     * promote to GOOD only when good_timer >= promote_after_good_s AND
//       bad_dwell >= min_bad_dwell_s; on promotion both timers reset to 0.
//
// A non-finite or non-positive `dt_s` is ignored (state left untouched), so a
// stalled / garbage clock can never corrupt the machine.
void congestion_update(CongestionState& s, const CongestionConfig& cfg,
                       f32 rtt_s, f32 dt_s) noexcept;

// Current send rate (Hz): good_send_hz in GOOD, bad_send_hz in BAD.
f32 congestion_send_hz(const CongestionState& s,
                       const CongestionConfig& cfg) noexcept;

// Current mode as a NetMode.
NetMode congestion_mode(const CongestionState& s) noexcept;

}  // namespace psynder::net
