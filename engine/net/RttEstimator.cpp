// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — smoothed RTT / jitter estimator. Lane 18 (net).
//
// See RttEstimator.h for the contract and the RFC 6298 / Jacobson-Karels
// rationale. Pure algebra (only std::fabs); no heap, no RNG, no transcendentals
// — bit-reproducible across platforms in the strict-FP net lane.

#include "net/RttEstimator.h"

#include "core/Types.h"

#include <cmath>

namespace psynder::net {

RttEstimator::RttEstimator() noexcept = default;

void RttEstimator::reset() noexcept {
    srtt_   = 0.0f;
    rttvar_ = 0.0f;
    has_    = false;
}

void RttEstimator::add_sample(f32 rtt_s) noexcept {
    // Reject garbage samples (NaN/Inf, or a negative trip time) — they would
    // poison the EWMA. State is left untouched so callers can ignore the drop.
    if (!std::isfinite(rtt_s) || rtt_s < 0.0f) {
        return;
    }

    if (!has_) {
        // RFC 6298 §2 (2.2): seed from the very first measurement.
        srtt_   = rtt_s;
        rttvar_ = rtt_s * 0.5f;  // rtt / 2
        has_    = true;
        return;
    }

    // RFC 6298 §2 (2.3): update the variation from the OLD srtt first, then
    // advance srtt. Order matters — rttvar must see the pre-update srtt.
    const f32 err = std::fabs(srtt_ - rtt_s);
    rttvar_ = (1.0f - kBeta) * rttvar_ + kBeta * err;
    srtt_   = (1.0f - kAlpha) * srtt_ + kAlpha * rtt_s;
}

f32 RttEstimator::rto(f32 min_s, f32 max_s) const noexcept {
    if (!has_) {
        return min_s;  // no estimate yet — fall back to the floor.
    }
    f32 v = srtt_ + kRtoVarMult * rttvar_;  // RFC 6298 RTO = SRTT + K*RTTVAR
    if (v < min_s) {
        v = min_s;
    }
    if (v > max_s) {
        v = max_s;
    }
    return v;
}

f32 RttEstimator::interp_delay_s(f32 safety_mult) const noexcept {
    if (!has_) {
        return 0.0f;
    }
    // Half-RTT one-way estimate plus a jitter margin scaled by safety_mult.
    return srtt_ * 0.5f + safety_mult * rttvar_;
}

}  // namespace psynder::net
