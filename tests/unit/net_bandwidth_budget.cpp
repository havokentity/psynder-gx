// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net per-tick send byte-budget tests.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "net/BandwidthBudget.h"

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net-budget: refill adds bytes_per_s times dt and caps at burst", "[net]") {
    BandwidthBudget b;
    b.configure(/*bytes_per_s=*/1000.0f, /*burst_bytes=*/500);

    // A fresh budget is empty.
    CHECK(b.available() == Catch::Approx(0.0f));

    // 0.1 s at 1000 B/s = 100 bytes, well under the 500-byte cap.
    b.refill(0.1f);
    CHECK(b.available() == Catch::Approx(100.0f));

    // Another 0.3 s = 300 bytes -> 400 total, still under the cap.
    b.refill(0.3f);
    CHECK(b.available() == Catch::Approx(400.0f));

    // 0.5 s = 500 more would reach 900, but the bucket saturates at burst 500.
    b.refill(0.5f);
    CHECK(b.available() == Catch::Approx(500.0f));

    // Further refills stay pinned at the cap.
    b.refill(10.0f);
    CHECK(b.available() == Catch::Approx(500.0f));
}

TEST_CASE("net-budget: can_spend and try_spend succeed within budget and fail beyond",
          "[net]") {
    BandwidthBudget b;
    b.configure(/*bytes_per_s=*/10000.0f, /*burst_bytes=*/1000);
    b.refill(0.05f);  // 500 bytes in the bucket
    REQUIRE(b.available() == Catch::Approx(500.0f));

    // Within budget: query then spend.
    CHECK(b.can_spend(200));
    CHECK(b.try_spend(200));
    CHECK(b.available() == Catch::Approx(300.0f));

    // Exactly the remaining budget spends fine.
    CHECK(b.can_spend(300));
    CHECK(b.try_spend(300));
    CHECK(b.available() == Catch::Approx(0.0f));

    // Now empty: nothing more can be spent.
    CHECK_FALSE(b.can_spend(1));
    CHECK_FALSE(b.try_spend(1));
}

TEST_CASE("net-budget: a failed spend leaves the bucket unchanged", "[net]") {
    BandwidthBudget b;
    b.configure(/*bytes_per_s=*/10000.0f, /*burst_bytes=*/1000);
    b.refill(0.01f);  // 100 bytes
    REQUIRE(b.available() == Catch::Approx(100.0f));

    // 150 > 100: must fail and NOT charge the bucket (all-or-nothing).
    CHECK_FALSE(b.can_spend(150));
    CHECK_FALSE(b.try_spend(150));
    CHECK(b.available() == Catch::Approx(100.0f));

    // The 100 bytes are still fully spendable after the failed attempt.
    CHECK(b.try_spend(100));
    CHECK(b.available() == Catch::Approx(0.0f));
}

TEST_CASE("net-budget: spending then refilling restores capacity", "[net]") {
    BandwidthBudget b;
    b.configure(/*bytes_per_s=*/2000.0f, /*burst_bytes=*/1000);
    b.refill(0.25f);  // 500 bytes
    REQUIRE(b.try_spend(500));
    CHECK(b.available() == Catch::Approx(0.0f));

    // A later tick pours more budget back in.
    b.refill(0.1f);  // 200 bytes
    CHECK(b.available() == Catch::Approx(200.0f));
    CHECK(b.try_spend(200));
    CHECK(b.available() == Catch::Approx(0.0f));
}

TEST_CASE("net-budget: a default unconfigured budget spends nothing", "[net]") {
    BandwidthBudget b;  // zero rate, zero cap, empty bucket

    CHECK(b.bytes_per_s() == Catch::Approx(0.0f));
    CHECK(b.burst_bytes() == 0u);
    CHECK(b.available() == Catch::Approx(0.0f));
    CHECK(b.available_bytes() == 0u);

    // Refilling does nothing without a rate and a cap.
    b.refill(5.0f);
    CHECK(b.available() == Catch::Approx(0.0f));

    // Even a single byte cannot be spent.
    CHECK_FALSE(b.can_spend(1));
    CHECK_FALSE(b.try_spend(1));

    // Spending zero bytes is trivially fine (the empty bucket holds >= 0).
    CHECK(b.can_spend(0));
    CHECK(b.try_spend(0));
}

TEST_CASE("net-budget: available_bytes floors the fractional fill", "[net]") {
    BandwidthBudget b;
    b.configure(/*bytes_per_s=*/1000.0f, /*burst_bytes=*/10000);

    // 0.0017 s * 1000 B/s = 1.7 bytes -> floor is 1.
    b.refill(0.0017f);
    CHECK(b.available() == Catch::Approx(1.7f));
    CHECK(b.available_bytes() == 1u);

    // Push it over to 2.7 bytes -> floor is 2.
    b.refill(0.001f);
    CHECK(b.available() == Catch::Approx(2.7f));
    CHECK(b.available_bytes() == 2u);
}

TEST_CASE("net-budget: a zero or non-finite dt leaves the bucket untouched", "[net]") {
    BandwidthBudget b;
    b.configure(/*bytes_per_s=*/1000.0f, /*burst_bytes=*/1000);
    b.refill(0.1f);  // 100 bytes
    REQUIRE(b.available() == Catch::Approx(100.0f));

    const f32 zero = 0.0f;
    const f32 nan  = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf  = std::numeric_limits<f32>::infinity();
    const f32 neg  = -0.5f;

    b.refill(zero);
    b.refill(nan);
    b.refill(inf);
    b.refill(neg);

    // None of the guarded steps changed the fill.
    CHECK(b.available() == Catch::Approx(100.0f));
}

TEST_CASE("net-budget: reset empties the bucket but keeps the configured rate and cap",
          "[net]") {
    BandwidthBudget b;
    b.configure(/*bytes_per_s=*/4000.0f, /*burst_bytes=*/800);
    b.refill(0.2f);  // 800 bytes (capped)
    REQUIRE(b.available() == Catch::Approx(800.0f));

    b.reset();
    CHECK(b.available() == Catch::Approx(0.0f));
    CHECK(b.available_bytes() == 0u);

    // Rate and cap survive the reset — the bucket refills again as configured.
    CHECK(b.bytes_per_s() == Catch::Approx(4000.0f));
    CHECK(b.burst_bytes() == 800u);
    b.refill(0.1f);  // 400 bytes
    CHECK(b.available() == Catch::Approx(400.0f));
}

TEST_CASE("net-budget: configure clamps a non-finite or negative rate to zero", "[net]") {
    BandwidthBudget b;
    b.configure(std::numeric_limits<f32>::quiet_NaN(), 1000);
    CHECK(b.bytes_per_s() == Catch::Approx(0.0f));
    b.refill(1.0f);
    CHECK(b.available() == Catch::Approx(0.0f));

    b.configure(-500.0f, 1000);
    CHECK(b.bytes_per_s() == Catch::Approx(0.0f));
    b.refill(1.0f);
    CHECK(b.available() == Catch::Approx(0.0f));
}

TEST_CASE("net-budget: shrinking the cap re-clamps the current fill immediately", "[net]") {
    BandwidthBudget b;
    b.configure(/*bytes_per_s=*/1000.0f, /*burst_bytes=*/1000);
    b.refill(0.8f);  // 800 bytes
    REQUIRE(b.available() == Catch::Approx(800.0f));

    // Reconfigure with a smaller cap: the 800-byte fill is clamped down to 500.
    b.configure(/*bytes_per_s=*/1000.0f, /*burst_bytes=*/500);
    CHECK(b.available() == Catch::Approx(500.0f));
}

TEST_CASE("net-budget: token-bucket scenario throttles to what has accumulated", "[net]") {
    // 30-byte records, a 100 B/s drip, a 60-byte burst cap, ticking at 0.1 s.
    BandwidthBudget b;
    b.configure(/*bytes_per_s=*/100.0f, /*burst_bytes=*/60);

    const usize record = 30;
    usize       sent   = 0;

    // Helper: one send tick — refill, then pack records until the budget is out.
    auto tick = [&]() {
        b.refill(0.1f);  // +10 bytes per tick
        while (b.try_spend(record)) {
            ++sent;
        }
    };

    // Tick 1: 10 bytes accumulated — cannot afford a 30-byte record yet.
    tick();
    CHECK(sent == 0u);
    CHECK(b.available() == Catch::Approx(10.0f));

    // Ticks 2,3: 20 then 30 bytes — at 30 we can finally ship exactly one
    // record, leaving the bucket empty.
    tick();
    CHECK(sent == 0u);
    tick();
    CHECK(sent == 1u);
    CHECK(b.available() == Catch::Approx(0.0f));

    // Let the bucket fill to the burst cap (drip is slow; cap is 60). After
    // several idle-pack ticks it saturates at 60 = two records' worth, so a
    // single tick can then ship two records back-to-back.
    for (int i = 0; i < 10; ++i) {
        b.refill(0.1f);
    }
    CHECK(b.available() == Catch::Approx(60.0f));  // saturated at burst

    usize burst_sent = 0;
    while (b.try_spend(record)) {
        ++burst_sent;
    }
    CHECK(burst_sent == 2u);  // 60 / 30 — never more than the bucket allows
    CHECK(b.available() == Catch::Approx(0.0f));
}

TEST_CASE("net-budget: identical config and op sequences yield identical bucket state",
          "[net][determinism]") {
    const f32   dts[]     = {0.013f, 0.1f, 0.0007f, 0.25f, 0.05f, 0.031f};
    const usize spends[]  = {17, 4, 256, 1, 99, 32};

    BandwidthBudget a;
    BandwidthBudget c;
    a.configure(5000.0f, 2048);
    c.configure(5000.0f, 2048);

    for (usize i = 0; i < 6; ++i) {
        a.refill(dts[i]);
        c.refill(dts[i]);
        const bool ra = a.try_spend(spends[i]);
        const bool rc = c.try_spend(spends[i]);
        CHECK(ra == rc);
        CHECK(a.available() == c.available());
        CHECK(a.available_bytes() == c.available_bytes());
    }

    CHECK(a.available() == Catch::Approx(c.available()));
}
