// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net client/server clock-offset synchronization tests.

#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/ClockSync.h"

using namespace psynder;
using namespace psynder::net;

namespace {

// Build a four-timestamp exchange for a known server-minus-client offset and
// (possibly asymmetric) one-way latencies. The client clock starts at `t0`;
// the server clock is the client clock plus `offset`.
//   t0 = client send
//   t1 = t0 + up   on the wire, read on the SERVER clock  => + offset
//   t2 = t1 + hold (server processing), server clock
//   t3 = t2 - offset + down, read back on the CLIENT clock
SyncSample make_sample(f64 t0, f64 offset, f64 up, f64 down,
                       f64 hold = 0.0) noexcept {
    SyncSample s;
    s.t0 = t0;
    s.t1 = (t0 + up) + offset;     // server-clock receive time
    s.t2 = s.t1 + hold;            // server-clock send time
    s.t3 = (s.t2 - offset) + down; // back on the client clock
    return s;
}

}  // namespace

TEST_CASE("net-clocksync: textbook rtt and offset for a symmetric exchange",
          "[net]") {
    // Known offset +5.0 s, symmetric 0.1 s RTT (0.05 s each way), no hold.
    const SyncSample s = make_sample(/*t0=*/1000.0, /*offset=*/5.0,
                                     /*up=*/0.05, /*down=*/0.05);
    CHECK(sample_offset(s) == Catch::Approx(5.0));
    CHECK(sample_rtt(s) == Catch::Approx(0.1));
}

TEST_CASE("net-clocksync: server hold time is excluded from rtt", "[net]") {
    // Same path latency, but the server sits on the request for 0.2 s. RTT is
    // the WIRE time only; the offset is unaffected by the hold.
    const SyncSample s = make_sample(/*t0=*/0.0, /*offset=*/2.0,
                                     /*up=*/0.03, /*down=*/0.03, /*hold=*/0.2);
    CHECK(sample_rtt(s) == Catch::Approx(0.06));
    CHECK(sample_offset(s) == Catch::Approx(2.0));
}

TEST_CASE("net-clocksync: fresh ClockSync has no estimate and reads zero",
          "[net]") {
    ClockSync cs;
    CHECK_FALSE(cs.has_estimate());
    CHECK(cs.sample_count() == 0u);
    CHECK(cs.offset() == Catch::Approx(0.0));
    CHECK(cs.best_rtt() == Catch::Approx(0.0));
    // Before any sample, server_time is the identity.
    CHECK(cs.server_time(42.0) == Catch::Approx(42.0));
}

TEST_CASE("net-clocksync: a single symmetric sample yields the exact offset",
          "[net]") {
    ClockSync cs;
    cs.add_sample(make_sample(/*t0=*/500.0, /*offset=*/5.0,
                              /*up=*/0.05, /*down=*/0.05));
    REQUIRE(cs.has_estimate());
    CHECK(cs.sample_count() == 1u);
    CHECK(cs.offset() == Catch::Approx(5.0));
    CHECK(cs.best_rtt() == Catch::Approx(0.1));
    // server_time(client) == client + offset.
    CHECK(cs.server_time(500.0) == Catch::Approx(505.0));
}

TEST_CASE("net-clocksync: offset tracks the lowest-RTT sample seen", "[net]") {
    ClockSync cs;

    // A high-RTT, jittery sample first (congested path, asymmetric queuing).
    cs.add_sample(make_sample(/*t0=*/0.0, /*offset=*/7.0,
                              /*up=*/0.20, /*down=*/0.40));  // rtt 0.60
    CHECK(cs.best_rtt() == Catch::Approx(0.60));

    // Then the cleanest, lowest-RTT sample — its offset must win.
    cs.add_sample(make_sample(/*t0=*/1.0, /*offset=*/5.0,
                              /*up=*/0.01, /*down=*/0.01));  // rtt 0.02
    CHECK(cs.best_rtt() == Catch::Approx(0.02));
    CHECK(cs.offset() == Catch::Approx(5.0));

    // A later high-RTT sample must NOT override the better one.
    cs.add_sample(make_sample(/*t0=*/2.0, /*offset=*/9.0,
                              /*up=*/0.15, /*down=*/0.30));  // rtt 0.45
    CHECK(cs.best_rtt() == Catch::Approx(0.02));
    CHECK(cs.offset() == Catch::Approx(5.0));

    CHECK(cs.sample_count() == 3u);
    CHECK(cs.server_time(2.0) == Catch::Approx(2.0 + 5.0));
}

TEST_CASE("net-clocksync: a symmetric sample is exact, asymmetric is bounded",
          "[net]") {
    // Symmetric: zero bias, exact recovery of the true offset.
    const SyncSample sym = make_sample(/*t0=*/0.0, /*offset=*/3.0,
                                       /*up=*/0.04, /*down=*/0.04);
    CHECK(sample_offset(sym) == Catch::Approx(3.0));

    // Asymmetric (up=0.02, down=0.08): the NTP estimate carries a bias of
    // (up - down)/2 = -0.03, so the estimate is true_offset - 0.03. Assert it
    // lands inside a tight band bounded by half the RTT.
    const f64 up    = 0.02;
    const f64 down  = 0.08;
    const f64 truth = 3.0;
    const SyncSample asym = make_sample(/*t0=*/0.0, truth, up, down);
    const f64 est  = sample_offset(asym);
    const f64 bias = (up - down) * 0.5;  // -0.03
    CHECK(est == Catch::Approx(truth + bias));
    // Boundedness: the error never exceeds half the RTT.
    const f64 half_rtt = sample_rtt(asym) * 0.5;
    CHECK(std::fabs(est - truth) <= half_rtt);
}

TEST_CASE("net-clocksync: non-finite and negative-rtt samples are ignored",
          "[net]") {
    ClockSync cs;
    cs.add_sample(make_sample(/*t0=*/0.0, /*offset=*/5.0,
                              /*up=*/0.05, /*down=*/0.05));  // valid seed
    const f64 off0   = cs.offset();
    const f64 rtt0   = cs.best_rtt();
    const u32 count0 = cs.sample_count();

    const f64 inf = std::numeric_limits<f64>::infinity();
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();

    SyncSample bad_nan;
    bad_nan.t0 = nan; bad_nan.t1 = 0.0; bad_nan.t2 = 0.0; bad_nan.t3 = 0.0;
    cs.add_sample(bad_nan);

    SyncSample bad_inf;
    bad_inf.t0 = 0.0; bad_inf.t1 = inf; bad_inf.t2 = 0.0; bad_inf.t3 = 0.0;
    cs.add_sample(bad_inf);

    // A physically-impossible negative RTT (t3 before t0 on the wire).
    SyncSample neg;
    neg.t0 = 10.0; neg.t1 = 10.0; neg.t2 = 10.0; neg.t3 = 9.0;  // rtt = -1
    cs.add_sample(neg);

    // State unchanged: every bad sample was dropped and did not count.
    CHECK(cs.offset() == Catch::Approx(off0));
    CHECK(cs.best_rtt() == Catch::Approx(rtt0));
    CHECK(cs.sample_count() == count0);
    CHECK(cs.has_estimate());
}

TEST_CASE("net-clocksync: reset clears all state", "[net]") {
    ClockSync cs;
    cs.add_sample(make_sample(0.0, 5.0, 0.05, 0.05));
    cs.add_sample(make_sample(1.0, 5.0, 0.02, 0.02));
    REQUIRE(cs.has_estimate());

    cs.reset();
    CHECK_FALSE(cs.has_estimate());
    CHECK(cs.sample_count() == 0u);
    CHECK(cs.offset() == Catch::Approx(0.0));
    CHECK(cs.best_rtt() == Catch::Approx(0.0));
    CHECK(cs.server_time(7.0) == Catch::Approx(7.0));  // identity again
}

TEST_CASE("net-clocksync: identical sample sequences are bit-identical",
          "[net][determinism]") {
    const std::vector<SyncSample> samples = {
        make_sample(0.0, 5.0, 0.040, 0.055),
        make_sample(1.0, 5.0, 0.010, 0.010),
        make_sample(2.0, 5.0, 0.090, 0.030),
        make_sample(3.0, 5.0, 0.020, 0.020),
        make_sample(4.0, 5.0, 0.062, 0.051),
        make_sample(5.0, 5.0, 0.005, 0.005),
    };

    auto run = [&samples]() {
        ClockSync cs;
        std::vector<f64> trace;
        trace.reserve(samples.size() * 2);
        for (const SyncSample& s : samples) {
            cs.add_sample(s);
            trace.push_back(cs.offset());
            trace.push_back(cs.best_rtt());
        }
        return trace;
    };

    const std::vector<f64> a = run();
    const std::vector<f64> b = run();

    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        // Exact bit equality, not approximate — same ops, same order.
        CHECK(a[i] == b[i]);
    }
}
