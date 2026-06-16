// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — client/server clock-offset synchronization. Lane 18 (net).
//
// The client and server run on independent wall clocks that disagree by some
// unknown offset (and drift slowly). Everything downstream of the network layer
// — the PlayoutClock render time (PlayoutClock.h), lag-compensation rewind
// (LagComp.h), and the lockstep schedule — is expressed in SERVER time. So the
// client must convert its own local time into server time, which means it must
// continuously estimate the offset between the two clocks.
//
// This is the classic NTP-style four-timestamp exchange (RFC 5905 §8):
//
//        t0 ─── client stamps its local time and sends a ping ───►
//                                                                  t1  server
//                                                                      receive
//                                                                  t2  server
//        ◄── client receives the reply, stamps t3 ───────────────     send
//
//   round-trip time  rtt    = (t3 - t0) - (t2 - t1)
//   clock offset     theta  = ((t1 - t0) + (t2 - t3)) / 2
//
// theta is "server clock minus client clock": server_time = client_time + theta.
// The derivation assumes the up-leg and down-leg latencies are equal; when they
// are not, theta carries a fixed bias of (down - up) / 2. There is no way to
// observe that asymmetry from the four timestamps alone, so it is accepted.
//
// Best-sample filter (the policy this class implements):
//   The (down - up) asymmetry bias is largest exactly when the path is congested
//   — queuing delay piles onto whichever leg is loaded, which ALSO inflates the
//   measured RTT. So the LOWEST-RTT sample seen is the one least corrupted by
//   queuing and therefore the most accurate offset estimate. ClockSync keeps the
//   offset from the lowest-RTT sample observed so far and reports it from
//   offset(); higher-RTT samples are recorded (for sample_count / best_rtt
//   bookkeeping) but never override a better one. This is the standard NTP
//   "clock filter" idea reduced to its essence: trust the cleanest round-trip.
//   (RttEstimator.h smooths RTT for timeouts — a DIFFERENT, complementary job;
//   this class does not duplicate that EWMA, it min-filters for accuracy.)
//
// Determinism: this is the strict-FP net lane (-fno-fast-math -ffp-contract=off,
// DESIGN-PSYNDER-GX.md §14). Pure f64 algebra (+, -, *, /, comparisons) — no
// RNG, no transcendentals — so the same sequence of samples yields a
// bit-identical offset on every platform. No per-call heap: the whole state is
// two doubles, a count and a bool.

#pragma once

#include "core/Types.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// SyncSample — one four-timestamp NTP exchange (all in seconds).
//   t0  client send time   (local clock)
//   t1  server receive time (server clock)
//   t2  server send  time  (server clock; frequently == t1)
//   t3  client receive time (local clock)
// ──────────────────────────────────────────────────────────────────────────
struct SyncSample {
    f64 t0 = 0.0;  // client-send   (client clock)
    f64 t1 = 0.0;  // server-recv   (server clock)
    f64 t2 = 0.0;  // server-send   (server clock)
    f64 t3 = 0.0;  // client-recv   (client clock)
};

// Round-trip time of the exchange (seconds): the total elapsed on the client
// minus the time the server spent holding the request.
//   rtt = (t3 - t0) - (t2 - t1)
f64 sample_rtt(const SyncSample& s) noexcept;

// Clock offset of the exchange (seconds), "server minus client":
//   theta = ((t1 - t0) + (t2 - t3)) / 2
f64 sample_offset(const SyncSample& s) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// ClockSync — accumulates NTP exchanges and keeps the offset from the
// lowest-RTT (cleanest) sample. See the file header for the best-sample policy
// and determinism contract.
// ──────────────────────────────────────────────────────────────────────────
class ClockSync {
public:
    ClockSync() noexcept;

    // Drop all history: has_estimate() is false again, offset() and best_rtt()
    // read zero, sample_count() is 0.
    void reset() noexcept;

    // Fold one exchange into the estimate. The RTT and offset are computed from
    // the sample; if its RTT is lower than the best seen so far (or this is the
    // first sample) the kept offset is replaced with this sample's offset.
    // Non-finite RTT/offset samples are ignored and do NOT count. The
    // sample_count() of accepted samples is always incremented for a valid one.
    void add_sample(const SyncSample& s) noexcept;

    // Best (lowest-RTT) clock offset seen so far, "server minus client"
    // (seconds). 0 before any sample.
    f64 offset() const noexcept { return best_offset_; }

    // The RTT (seconds) of the lowest-RTT sample whose offset is reported.
    // 0 before any sample.
    f64 best_rtt() const noexcept { return best_rtt_; }

    // Number of valid samples folded in since construction / reset.
    u32 sample_count() const noexcept { return count_; }

    // Has at least one valid sample been recorded?
    bool has_estimate() const noexcept { return has_; }

    // Convert a client-clock time (seconds) to server-clock time using the best
    // offset: server_time = client_time + offset(). Before any sample the offset
    // is 0, so this is the identity — callers should gate on has_estimate().
    f64 server_time(f64 client_time) const noexcept;

private:
    f64  best_offset_ = 0.0;    // offset from the lowest-RTT sample
    f64  best_rtt_    = 0.0;    // that sample's RTT (the running minimum)
    u32  count_       = 0;      // valid samples folded in
    bool has_         = false;  // any valid sample yet?
};

}  // namespace psynder::net
