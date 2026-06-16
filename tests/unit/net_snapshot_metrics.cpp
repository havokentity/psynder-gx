// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net snapshot bandwidth-metrics tests.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "net/SnapshotMetrics.h"
#include "net/SnapshotPack.h"

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net-metrics: record accumulates total, count, peak and mean", "[net]") {
    BandwidthMeter m{};

    // Three frames; peak is the middle one.
    meter_record(m, 100);
    meter_record(m, 250);
    meter_record(m, 150);

    CHECK(m.total_bytes == 500u);      // 100 + 250 + 150
    CHECK(m.frames == 3u);
    CHECK(m.peak_frame_bytes == 250u); // the max frame
    CHECK(mean_bytes_per_tick(m) == Catch::Approx(500.0 / 3.0));
}

TEST_CASE("net-metrics: rates scale the mean by tickrate and bits are 8x bytes",
          "[net]") {
    BandwidthMeter m{};
    // Two frames averaging 200 bytes/tick.
    meter_record(m, 150);
    meter_record(m, 250);
    REQUIRE(mean_bytes_per_tick(m) == Catch::Approx(200.0));

    const f32 tickrate = 64.0f;
    CHECK(bytes_per_second(m, tickrate) == Catch::Approx(200.0 * 64.0));
    CHECK(bits_per_second(m, tickrate) ==
          Catch::Approx(200.0 * 64.0 * 8.0));
    // bits are exactly 8x bytes at the same tickrate.
    CHECK(bits_per_second(m, tickrate) ==
          Catch::Approx(bytes_per_second(m, tickrate) * 8.0));
}

TEST_CASE("net-metrics: an empty meter returns zero for every rate (no div-by-zero)",
          "[net]") {
    BandwidthMeter m{};  // no frames recorded

    CHECK(mean_bytes_per_tick(m) == Catch::Approx(0.0));
    CHECK(bytes_per_second(m, 128.0f) == Catch::Approx(0.0));
    CHECK(bits_per_second(m, 128.0f) == Catch::Approx(0.0));
}

TEST_CASE("net-metrics: reset zeroes every field", "[net]") {
    BandwidthMeter m{};
    meter_record(m, 999);
    meter_record(m, 1);
    REQUIRE(m.frames == 2u);

    meter_reset(m);
    CHECK(m.total_bytes == 0u);
    CHECK(m.frames == 0u);
    CHECK(m.peak_frame_bytes == 0u);
    // And derived rates fall back to zero with no frames.
    CHECK(mean_bytes_per_tick(m) == Catch::Approx(0.0));
}

TEST_CASE("net-metrics: full_snapshot_bytes mirrors net::packed_size", "[net]") {
    for (usize n : {usize{0}, usize{1}, usize{100}, usize{1000}}) {
        CHECK(full_snapshot_bytes(n) == net::packed_size(n));
    }
    // Spot-check the documented closed form: 4 + 20 * n.
    CHECK(full_snapshot_bytes(100) == 4u + 20u * 100u);
}

TEST_CASE("net-metrics: a small delta vs a 100-entity full snapshot compresses well",
          "[net]") {
    const usize full = full_snapshot_bytes(100);  // 2004 bytes
    // A delta carrying just a couple of changed records is a tiny fraction.
    const usize delta = 8 + 2 * 20;               // 48 bytes

    const f64 ratio = compression_ratio(delta, full);
    CHECK(ratio == Catch::Approx(static_cast<f64>(delta) /
                                 static_cast<f64>(full)));
    CHECK(ratio < 1.0);  // delta is well under a full snapshot

    const f64 saved = savings_fraction(delta, full);
    CHECK(saved == Catch::Approx(1.0 - ratio));
    CHECK(saved > 0.9);  // over 90% saved
}

TEST_CASE("net-metrics: compression_ratio with a zero full snapshot returns 1.0",
          "[net]") {
    // No baseline to compare against — guarded divide returns 1.0 (no
    // compression) and savings is therefore 0.
    CHECK(compression_ratio(48, 0) == Catch::Approx(1.0));
    CHECK(savings_fraction(48, 0) == Catch::Approx(0.0));
}

TEST_CASE("net-metrics: a delta larger than the full snapshot reports >1 and negative savings",
          "[net]") {
    // Pathological case: the delta exceeds the full snapshot. Intentionally NOT
    // clamped, so the regression stays visible.
    const usize full  = full_snapshot_bytes(1);  // 24 bytes
    const usize delta = 100;

    CHECK(compression_ratio(delta, full) > 1.0);
    CHECK(savings_fraction(delta, full) < 0.0);
}

TEST_CASE("net-metrics: identical record sequences yield identical totals and means",
          "[net][determinism]") {
    const usize frames[] = {17, 4, 256, 1, 99, 32};

    BandwidthMeter a{};
    BandwidthMeter b{};
    for (usize f : frames) {
        meter_record(a, f);
        meter_record(b, f);
    }

    CHECK(a.total_bytes == b.total_bytes);
    CHECK(a.frames == b.frames);
    CHECK(a.peak_frame_bytes == b.peak_frame_bytes);
    CHECK(mean_bytes_per_tick(a) == mean_bytes_per_tick(b));
    CHECK(bits_per_second(a, 60.0f) == bits_per_second(b, 60.0f));
}
