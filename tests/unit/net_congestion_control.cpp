// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net: adaptive send-rate congestion control tests.
//
// Covers the Gaffer good/bad-mode machine in CongestionControl.h: init starts
// GOOD at the high rate; an unhealthy RTT demotes to BAD immediately at the low
// rate; recovery is gated by BOTH the contiguous-good window AND the minimum
// bad dwell (hysteresis); a bad reading mid-recovery resets the good timer; a
// zero/garbage dt is ignored; and the whole machine is deterministic.

#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "net/CongestionControl.h"

using namespace psynder;
using namespace psynder::net;

namespace {

// A config with round numbers so the dwell/recovery arithmetic is obvious:
//   threshold 100 ms, 30 Hz good / 10 Hz bad,
//   promote after 1.0 s contiguous-good, min bad dwell 0.5 s.
constexpr CongestionConfig kCfg{
    /*rtt_bad_threshold_s =*/0.100f,
    /*good_send_hz        =*/30.0f,
    /*bad_send_hz         =*/10.0f,
    /*promote_after_good_s=*/1.0f,
    /*min_bad_dwell_s     =*/0.5f,
};

constexpr f32 kDt = 0.1f;  // 100 ms steps.
constexpr f32 kGoodRtt = 0.050f;  // well under threshold -> healthy.
constexpr f32 kBadRtt  = 0.200f;  // well over threshold  -> unhealthy.

}  // namespace

TEST_CASE("net: congestion init is Good at the good send rate", "[net][congestion]") {
    CongestionState s;
    congestion_init(s);
    CHECK(congestion_mode(s) == NetMode::Good);
    CHECK(congestion_send_hz(s, kCfg) == Catch::Approx(kCfg.good_send_hz));
    CHECK(s.good_timer_s == Catch::Approx(0.0f));
    CHECK(s.bad_dwell_s == Catch::Approx(0.0f));
}

TEST_CASE("net: congestion staying healthy keeps Good mode", "[net][congestion]") {
    CongestionState s;
    congestion_init(s);
    for (int i = 0; i < 20; ++i) {
        congestion_update(s, kCfg, kGoodRtt, kDt);
        CHECK(congestion_mode(s) == NetMode::Good);
    }
    CHECK(congestion_send_hz(s, kCfg) == Catch::Approx(kCfg.good_send_hz));
}

TEST_CASE("net: congestion bad RTT demotes to Bad immediately at the bad rate",
          "[net][congestion]") {
    CongestionState s;
    congestion_init(s);
    // A single unhealthy reading from GOOD drops straight to BAD.
    congestion_update(s, kCfg, kBadRtt, kDt);
    CHECK(congestion_mode(s) == NetMode::Bad);
    CHECK(congestion_send_hz(s, kCfg) == Catch::Approx(kCfg.bad_send_hz));
    // Fresh dwell/recovery window on demotion.
    CHECK(s.bad_dwell_s == Catch::Approx(0.0f));
    CHECK(s.good_timer_s == Catch::Approx(0.0f));
}

TEST_CASE("net: congestion does not promote until both good window and bad dwell elapse",
          "[net][congestion]") {
    CongestionState s;
    congestion_init(s);
    congestion_update(s, kCfg, kBadRtt, kDt);  // -> Bad
    REQUIRE(congestion_mode(s) == NetMode::Bad);

    // promote_after_good_s = 1.0 s (10 healthy steps); min_bad_dwell_s = 0.5 s.
    // The good window (1.0 s) is the binding gate here — once it is satisfied
    // the dwell floor (0.5 s) is already comfortably exceeded.
    for (int i = 0; i < 9; ++i) {
        congestion_update(s, kCfg, kGoodRtt, kDt);
        // 9 healthy steps = 0.9 s contiguous good — short of the 1.0 s window,
        // so still BAD even though the 0.5 s dwell floor is long since cleared.
        CHECK(congestion_mode(s) == NetMode::Bad);
    }
    // The 10th healthy step reaches 1.0 s contiguous good AND > 0.5 s dwell.
    congestion_update(s, kCfg, kGoodRtt, kDt);
    CHECK(congestion_mode(s) == NetMode::Good);
    CHECK(congestion_send_hz(s, kCfg) == Catch::Approx(kCfg.good_send_hz));
}

TEST_CASE("net: congestion min bad dwell holds promotion even after the good window",
          "[net][congestion]") {
    // Config where the dwell floor is the binding gate: a tiny good window but a
    // long minimum dwell. Health is satisfied immediately, yet we must remain in
    // BAD until min_bad_dwell_s of total time has passed.
    constexpr CongestionConfig dwellCfg{
        /*rtt_bad_threshold_s =*/0.100f,
        /*good_send_hz        =*/30.0f,
        /*bad_send_hz         =*/10.0f,
        /*promote_after_good_s=*/0.1f,  // one healthy step satisfies the window.
        /*min_bad_dwell_s     =*/1.0f,  // but we owe a full second of dwell.
    };
    CongestionState s;
    congestion_init(s);
    congestion_update(s, dwellCfg, kBadRtt, kDt);  // -> Bad
    REQUIRE(congestion_mode(s) == NetMode::Bad);

    // After 9 healthy steps (0.9 s): the 0.1 s good window is met on step 1, but
    // total dwell is only 0.9 s (< 1.0 s) so the dwell floor pins us in BAD.
    for (int i = 0; i < 9; ++i) {
        congestion_update(s, dwellCfg, kGoodRtt, kDt);
        CHECK(congestion_mode(s) == NetMode::Bad);
    }
    // 10th step: dwell reaches 1.0 s and the good window is long satisfied.
    congestion_update(s, dwellCfg, kGoodRtt, kDt);
    CHECK(congestion_mode(s) == NetMode::Good);
}

TEST_CASE("net: congestion bad RTT during recovery resets the good timer",
          "[net][congestion]") {
    CongestionState s;
    congestion_init(s);
    congestion_update(s, kCfg, kBadRtt, kDt);  // -> Bad

    // Build up 0.9 s of contiguous good (just short of the 1.0 s window).
    for (int i = 0; i < 9; ++i) congestion_update(s, kCfg, kGoodRtt, kDt);
    REQUIRE(congestion_mode(s) == NetMode::Bad);
    REQUIRE(s.good_timer_s == Catch::Approx(0.9f));

    // A single bad reading mid-recovery wipes the contiguous-good progress.
    congestion_update(s, kCfg, kBadRtt, kDt);
    CHECK(congestion_mode(s) == NetMode::Bad);
    CHECK(s.good_timer_s == Catch::Approx(0.0f));

    // Now a single healthy step must NOT promote — we are back at 0.1 s good,
    // nowhere near the 1.0 s window (no premature promotion).
    congestion_update(s, kCfg, kGoodRtt, kDt);
    CHECK(congestion_mode(s) == NetMode::Bad);

    // It takes a fresh full contiguous-good window (9 more steps -> 1.0 s) to
    // finally promote.
    for (int i = 0; i < 9; ++i) congestion_update(s, kCfg, kGoodRtt, kDt);
    CHECK(congestion_mode(s) == NetMode::Good);
}

TEST_CASE("net: congestion ignores a zero or non-finite dt", "[net][congestion]") {
    CongestionState s;
    congestion_init(s);
    congestion_update(s, kCfg, kBadRtt, kDt);  // -> Bad
    congestion_update(s, kCfg, kGoodRtt, kDt);  // 0.1 s good
    const f32 dwell_before = s.bad_dwell_s;
    const f32 good_before = s.good_timer_s;

    // Zero dt: no-op.
    congestion_update(s, kCfg, kGoodRtt, 0.0f);
    CHECK(s.bad_dwell_s == Catch::Approx(dwell_before));
    CHECK(s.good_timer_s == Catch::Approx(good_before));

    // Negative dt: no-op.
    congestion_update(s, kCfg, kGoodRtt, -0.5f);
    CHECK(s.bad_dwell_s == Catch::Approx(dwell_before));
    CHECK(s.good_timer_s == Catch::Approx(good_before));

    // NaN dt: no-op.
    const f32 nan_dt = std::numeric_limits<f32>::quiet_NaN();
    congestion_update(s, kCfg, kGoodRtt, nan_dt);
    CHECK(s.bad_dwell_s == Catch::Approx(dwell_before));
    CHECK(s.good_timer_s == Catch::Approx(good_before));

    // Infinite dt: no-op.
    const f32 inf_dt = std::numeric_limits<f32>::infinity();
    congestion_update(s, kCfg, kGoodRtt, inf_dt);
    CHECK(s.bad_dwell_s == Catch::Approx(dwell_before));
    CHECK(s.good_timer_s == Catch::Approx(good_before));

    // Mode unchanged through all of the ignored ticks.
    CHECK(congestion_mode(s) == NetMode::Bad);
}

TEST_CASE("net: congestion boundary RTT exactly at the threshold counts as healthy",
          "[net][congestion]") {
    CongestionState s;
    congestion_init(s);
    // healthy := rtt <= threshold, so rtt == threshold must stay GOOD.
    congestion_update(s, kCfg, kCfg.rtt_bad_threshold_s, kDt);
    CHECK(congestion_mode(s) == NetMode::Good);
}

TEST_CASE("net: congestion is deterministic for an identical input sequence",
          "[net][congestion]") {
    // A fixed, hand-rolled RTT/dt script with demotions, partial recoveries,
    // resets, and a full promotion. Two independent runs must agree bit-for-bit
    // on mode, send rate, and both timers at every step.
    const f32 rtt_script[] = {
        kBadRtt, kGoodRtt, kGoodRtt, kBadRtt, kGoodRtt, kGoodRtt, kGoodRtt,
        kGoodRtt, kGoodRtt, kGoodRtt, kGoodRtt, kGoodRtt, kGoodRtt, kBadRtt,
        kGoodRtt, kGoodRtt,
    };

    CongestionState a;
    CongestionState b;
    congestion_init(a);
    congestion_init(b);

    for (f32 rtt : rtt_script) {
        congestion_update(a, kCfg, rtt, kDt);
        congestion_update(b, kCfg, rtt, kDt);
        CHECK(a.mode == b.mode);
        CHECK(a.good_timer_s == b.good_timer_s);  // bit-exact: same algebra.
        CHECK(a.bad_dwell_s == b.bad_dwell_s);
        CHECK(congestion_send_hz(a, kCfg) == congestion_send_hz(b, kCfg));
        CHECK(congestion_mode(a) == congestion_mode(b));
    }
}
