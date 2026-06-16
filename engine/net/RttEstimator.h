// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — smoothed RTT / jitter estimator. Lane 18 (net).
//
// The reliability layer (Reliability.h / AckTracker) turns ack masks into raw
// round-trip-time samples (the gap between when a reliable frame was sent and
// when its ack arrived). Those raw samples are noisy, so feeding them straight
// into a retransmission timeout would either retransmit too eagerly (wasting
// bandwidth) or too late (stalling the window). This estimator smooths the raw
// stream into a stable RTT estimate, an RTT-variation (jitter) estimate, and a
// retransmission timeout — the standard Jacobson/Karels algorithm from
// RFC 6298, the same one TCP uses.
//
// It also exposes a suggested client interpolation delay: the entity-interp
// buffer on the client must lag the server by at least the one-way trip plus a
// jitter margin so that there is always a future snapshot to interpolate toward.
//
// Determinism: this is the strict-FP net lane (-fno-fast-math
// -ffp-contract=off, DESIGN-PSYNDER-GX.md §14). The estimator is pure algebra
// (the only library call is std::fabs) — no RNG, no transcendentals — so the
// same sequence of samples yields a bit-identical estimate on every platform.
// No per-call heap: the whole state is two floats and a bool.

#pragma once

#include "core/Types.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// RttEstimator — Jacobson/Karels EWMA over raw RTT samples (RFC 6298).
//
//   first sample:  srtt = rtt;  rttvar = rtt / 2
//   thereafter:    rttvar = (1 - beta) * rttvar + beta * |srtt - rtt|
//                  srtt   = (1 - alpha) * srtt   + alpha * rtt
//
// with alpha = 1/8 and beta = 1/4 (the RFC's recommended gains). Note that
// rttvar is updated from the OLD srtt before srtt itself is advanced, exactly
// as the RFC specifies.
// ──────────────────────────────────────────────────────────────────────────
class RttEstimator {
public:
    // RFC 6298 gains. alpha smooths the mean RTT; beta smooths its variation.
    static constexpr f32 kAlpha = 1.0f / 8.0f;  // SRTT  EWMA gain
    static constexpr f32 kBeta  = 1.0f / 4.0f;  // RTTVAR EWMA gain
    // RFC 6298 §2 RTO multiplier on the variation term (K = 4).
    static constexpr f32 kRtoVarMult = 4.0f;

    // Uninitialised until the first sample.
    RttEstimator() noexcept;

    // Drop all history; has_sample() is false again afterwards.
    void reset() noexcept;

    // Fold one raw round-trip-time sample (seconds) into the estimate.
    // Non-finite (NaN/Inf) or negative samples are ignored — the estimator
    // state is left unchanged.
    void add_sample(f32 rtt_s) noexcept;

    // Smoothed round-trip time (seconds). 0 before any sample.
    f32 srtt() const noexcept { return srtt_; }

    // Smoothed RTT variation (seconds) — a jitter proxy. 0 before any sample.
    f32 rttvar() const noexcept { return rttvar_; }

    // Retransmission timeout (seconds) = clamp(srtt + 4*rttvar, min_s, max_s),
    // the RFC 6298 RTO formula. Returns min_s before any sample has arrived.
    f32 rto(f32 min_s, f32 max_s) const noexcept;

    // Suggested client interpolation delay (seconds): srtt/2 + safety_mult *
    // rttvar — the half-RTT one-way estimate plus a jitter margin sized by
    // `safety_mult` (e.g. 2..3). 0 before any sample.
    f32 interp_delay_s(f32 safety_mult) const noexcept;

    // Has at least one valid sample been folded in?
    bool has_sample() const noexcept { return has_; }

private:
    f32  srtt_   = 0.0f;
    f32  rttvar_ = 0.0f;
    bool has_    = false;
};

}  // namespace psynder::net
