// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — client/server clock-offset synchronization. Lane 18 (net).
//
// See ClockSync.h for the contract, the NTP four-timestamp derivation, and the
// best-sample (lowest-RTT) filter rationale. Pure f64 algebra (the only library
// call is std::isfinite for input validation); no heap, no RNG, no
// transcendentals — bit-reproducible across platforms in the strict-FP net lane.

#include "net/ClockSync.h"

#include "core/Types.h"

#include <cmath>

namespace psynder::net {

f64 sample_rtt(const SyncSample& s) noexcept {
    // Total time on the client wire minus the server's hold time. The two
    // server stamps (t1, t2) share a clock, as do the two client stamps
    // (t0, t3), so each difference is offset-free and rtt is offset-independent.
    return (s.t3 - s.t0) - (s.t2 - s.t1);
}

f64 sample_offset(const SyncSample& s) noexcept {
    // Average of the up-leg apparent offset (t1 - t0) and the down-leg apparent
    // offset (t2 - t3). Under equal up/down latency the latency terms cancel and
    // this is the exact "server minus client" offset.
    return ((s.t1 - s.t0) + (s.t2 - s.t3)) * 0.5;
}

ClockSync::ClockSync() noexcept = default;

void ClockSync::reset() noexcept {
    best_offset_ = 0.0;
    best_rtt_    = 0.0;
    count_       = 0;
    has_         = false;
}

void ClockSync::add_sample(const SyncSample& s) noexcept {
    const f64 rtt    = sample_rtt(s);
    const f64 offset = sample_offset(s);

    // Reject garbage exchanges (NaN/Inf timestamps propagate into rtt/offset).
    // A negative RTT is physically impossible and signals a bad/clock-warped
    // sample, so drop it too. Dropped samples do not count.
    if (!std::isfinite(rtt) || !std::isfinite(offset) || rtt < 0.0) {
        return;
    }

    // Keep the offset from the lowest-RTT sample: the cleanest round-trip is the
    // least corrupted by queuing asymmetry, hence the most accurate offset.
    if (!has_ || rtt < best_rtt_) {
        best_rtt_    = rtt;
        best_offset_ = offset;
        has_         = true;
    }

    ++count_;
}

f64 ClockSync::server_time(f64 client_time) const noexcept {
    // server_time = client_time + offset(). Offset is 0 before any sample, so
    // this is the identity until an estimate exists.
    return client_time + best_offset_;
}

}  // namespace psynder::net
