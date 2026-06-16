// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — per-tick send byte budget. Lane 18 (net).
//
// See BandwidthBudget.h for the token-bucket contract and the (send rate * MTU)
// rationale. Pure algebra (only a std::isfinite guard); the fill is accumulated
// in f64 so a slow refill does not round away, spends are integer byte counts.
// No heap, no RNG, no transcendentals — bit-reproducible across platforms in
// the strict-FP net lane.

#include "net/BandwidthBudget.h"

#include "core/Types.h"

#include <cmath>

namespace psynder::net {

// A refill of "100 bytes" is `rate * dt` in floating point, which does not land
// exactly on an integer (e.g. 10000 * 0.01 != 100 exactly). Tolerate well under
// one byte of error so a bucket nominally holding N bytes can actually spend N,
// and snap a near-empty bucket to exactly 0 so callers see a clean drained state.
static constexpr f64 kSpendEpsilon = 1.0e-3;

BandwidthBudget::BandwidthBudget() noexcept
    : fill_bytes_(0.0), bytes_per_s_(0.0f), burst_bytes_(0) {}

void BandwidthBudget::configure(f32 bytes_per_s, usize burst_bytes) noexcept {
    // A garbage or negative rate is meaningless; clamp it to "no refill" rather
    // than let a NaN poison every future fill.
    bytes_per_s_ = (std::isfinite(bytes_per_s) && bytes_per_s > 0.0f)
                       ? bytes_per_s
                       : 0.0f;
    burst_bytes_ = burst_bytes;

    // Re-clamp the current fill to the (possibly smaller) new cap so a shrink
    // takes effect immediately instead of leaving stale over-cap budget.
    const f64 cap = static_cast<f64>(burst_bytes_);
    if (fill_bytes_ > cap) {
        fill_bytes_ = cap;
    }
}

void BandwidthBudget::reset() noexcept { fill_bytes_ = 0.0; }

void BandwidthBudget::refill(f32 dt_s) noexcept {
    // Guard the clock: a NaN/Inf or non-positive step is meaningless and would
    // poison the bucket, so skip it entirely and leave the fill untouched.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) {
        return;
    }

    fill_bytes_ += static_cast<f64>(bytes_per_s_) * static_cast<f64>(dt_s);

    // Saturate at the burst cap — unspent budget carries forward, but never
    // past the bucket size.
    const f64 cap = static_cast<f64>(burst_bytes_);
    if (fill_bytes_ > cap) {
        fill_bytes_ = cap;
    }
}

bool BandwidthBudget::can_spend(usize bytes) const noexcept {
    // Sub-byte float-refill error must not stop a nominal-N bucket spending N.
    return fill_bytes_ >= static_cast<f64>(bytes) - kSpendEpsilon;
}

bool BandwidthBudget::try_spend(usize bytes) noexcept {
    // All-or-nothing: only charge the bucket when the whole record fits, so a
    // partial entity is never billed.
    if (!can_spend(bytes)) {
        return false;
    }
    fill_bytes_ -= static_cast<f64>(bytes);
    // Snap a drained-within-epsilon bucket (tiny + or - residue) to exactly 0.
    if (fill_bytes_ < kSpendEpsilon) {
        fill_bytes_ = 0.0;
    }
    return true;
}

f32 BandwidthBudget::available() const noexcept {
    return static_cast<f32>(fill_bytes_);
}

usize BandwidthBudget::available_bytes() const noexcept {
    // fill_bytes_ is always in [0, burst], so the floor is a clean,
    // non-negative byte count.
    return static_cast<usize>(std::floor(fill_bytes_));
}

}  // namespace psynder::net
