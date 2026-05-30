// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net smoothed RTT / jitter estimator tests (RFC 6298).

#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/RttEstimator.h"

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net-rtt: fresh estimator has no sample and zero estimates", "[net]") {
    RttEstimator e;
    CHECK_FALSE(e.has_sample());
    CHECK(e.srtt() == Catch::Approx(0.0f));
    CHECK(e.rttvar() == Catch::Approx(0.0f));
    // Before any sample, interp delay is zero and rto falls back to min.
    CHECK(e.interp_delay_s(2.0f) == Catch::Approx(0.0f));
    CHECK(e.rto(0.2f, 1.0f) == Catch::Approx(0.2f));
}

TEST_CASE("net-rtt: first sample seeds srtt=rtt and rttvar=rtt/2", "[net]") {
    RttEstimator e;
    e.add_sample(0.080f);  // 80 ms
    REQUIRE(e.has_sample());
    CHECK(e.srtt() == Catch::Approx(0.080f));
    CHECK(e.rttvar() == Catch::Approx(0.040f));  // rtt / 2
}

TEST_CASE("net-rtt: constant stream converges srtt to the value and rttvar to 0",
          "[net]") {
    RttEstimator e;
    const f32 rtt = 0.050f;  // 50 ms, perfectly steady
    for (int i = 0; i < 200; ++i) {
        e.add_sample(rtt);
    }
    // srtt sits on the value; rttvar decays toward zero (errors are all 0).
    CHECK(e.srtt() == Catch::Approx(rtt).epsilon(0.001));
    CHECK(e.rttvar() == Catch::Approx(0.0f).margin(1e-4f));
}

TEST_CASE("net-rtt: a noisy stream raises rttvar versus a steady one", "[net]") {
    RttEstimator steady;
    RttEstimator noisy;

    // Same mean (~50 ms), but the noisy stream swings ±30 ms each sample.
    const f32 mean = 0.050f;
    for (int i = 0; i < 200; ++i) {
        steady.add_sample(mean);
        const f32 jit = (i % 2 == 0) ? 0.030f : -0.030f;
        noisy.add_sample(mean + jit);
    }

    CHECK(noisy.rttvar() > steady.rttvar());
    // The noisy variation must be clearly non-trivial.
    CHECK(noisy.rttvar() > 0.005f);
}

TEST_CASE("net-rtt: rto = clamp(srtt + 4*rttvar, min, max)", "[net]") {
    RttEstimator e;
    e.add_sample(0.100f);  // srtt = 0.1, rttvar = 0.05
    // Raw RTO = 0.1 + 4*0.05 = 0.30, inside a wide [0.05, 1.0] band.
    CHECK(e.rto(0.05f, 1.0f) == Catch::Approx(0.30f));

    // Clamps at the HIGH end: cap below the raw 0.30.
    CHECK(e.rto(0.05f, 0.20f) == Catch::Approx(0.20f));

    // Clamps at the LOW end: floor above the raw 0.30.
    CHECK(e.rto(0.50f, 1.0f) == Catch::Approx(0.50f));
}

TEST_CASE("net-rtt: rto returns min before any sample", "[net]") {
    RttEstimator e;
    CHECK(e.rto(0.25f, 2.0f) == Catch::Approx(0.25f));
}

TEST_CASE("net-rtt: non-finite and negative samples are ignored", "[net]") {
    RttEstimator e;
    e.add_sample(0.060f);  // valid seed
    const f32 srtt0   = e.srtt();
    const f32 rttvar0 = e.rttvar();

    const f32 inf = std::numeric_limits<f32>::infinity();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    e.add_sample(inf);
    e.add_sample(-inf);
    e.add_sample(nan);
    e.add_sample(-0.010f);  // negative trip time

    // State unchanged: every bad sample was dropped.
    CHECK(e.srtt() == Catch::Approx(srtt0));
    CHECK(e.rttvar() == Catch::Approx(rttvar0));
    CHECK(e.has_sample());
}

TEST_CASE("net-rtt: a bad first sample leaves the estimator uninitialised",
          "[net]") {
    RttEstimator e;
    e.add_sample(std::numeric_limits<f32>::quiet_NaN());
    e.add_sample(-1.0f);
    CHECK_FALSE(e.has_sample());
    CHECK(e.srtt() == Catch::Approx(0.0f));
    CHECK(e.rttvar() == Catch::Approx(0.0f));
}

TEST_CASE("net-rtt: interp_delay grows with jitter", "[net]") {
    RttEstimator steady;
    RttEstimator noisy;

    const f32 mean = 0.050f;
    for (int i = 0; i < 200; ++i) {
        steady.add_sample(mean);
        const f32 jit = (i % 2 == 0) ? 0.030f : -0.030f;
        noisy.add_sample(mean + jit);
    }

    const f32 mult = 2.0f;
    // interp_delay = srtt/2 + mult*rttvar; equal-ish srtt, so larger rttvar
    // (noisy) yields the larger delay.
    CHECK(noisy.interp_delay_s(mult) > steady.interp_delay_s(mult));

    // Hand-checked seed value: one sample -> srtt=0.08, rttvar=0.04.
    RttEstimator one;
    one.add_sample(0.080f);
    CHECK(one.interp_delay_s(2.0f) ==
          Catch::Approx(0.080f * 0.5f + 2.0f * 0.040f));  // 0.04 + 0.08 = 0.12
}

TEST_CASE("net-rtt: reset clears all state", "[net]") {
    RttEstimator e;
    e.add_sample(0.070f);
    e.add_sample(0.090f);
    REQUIRE(e.has_sample());

    e.reset();
    CHECK_FALSE(e.has_sample());
    CHECK(e.srtt() == Catch::Approx(0.0f));
    CHECK(e.rttvar() == Catch::Approx(0.0f));
    CHECK(e.rto(0.2f, 1.0f) == Catch::Approx(0.2f));  // back to the floor
}

TEST_CASE("net-rtt: identical sample sequences are bit-identical",
          "[net][determinism]") {
    const std::vector<f32> samples = {
        0.040f, 0.055f, 0.048f, 0.090f, 0.030f, 0.062f,
        0.051f, 0.075f, 0.044f, 0.058f, 0.049f, 0.067f,
    };

    auto run = [&samples]() {
        RttEstimator e;
        std::vector<f32> trace;
        trace.reserve(samples.size() * 2);
        for (f32 s : samples) {
            e.add_sample(s);
            trace.push_back(e.srtt());
            trace.push_back(e.rttvar());
        }
        return trace;
    };

    const std::vector<f32> a = run();
    const std::vector<f32> b = run();

    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        // Exact bit equality, not approximate — same ops, same order.
        CHECK(a[i] == b[i]);
    }
}
