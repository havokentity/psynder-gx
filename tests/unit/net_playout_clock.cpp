// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net client playout (interpolation) clock tests.

#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/PlayoutClock.h"

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net-playout: before any snapshot has_snapshots is false and clocks read zero",
          "[net]") {
    PlayoutClock clk;
    CHECK_FALSE(clk.has_snapshots());
    CHECK(clk.render_time_s() == Catch::Approx(0.0));
    CHECK(clk.latest_server_time_s() == Catch::Approx(0.0));
}

TEST_CASE("net-playout: the first snapshot initialises render_time to server_time minus delay",
          "[net]") {
    PlayoutClock clk;
    clk.configure(0.1f);  // 100 ms interpolation delay

    clk.on_snapshot(10.0);
    CHECK(clk.has_snapshots());
    CHECK(clk.latest_server_time_s() == Catch::Approx(10.0));
    // render_time starts exactly one delay behind the freshest data.
    CHECK(clk.render_time_s() == Catch::Approx(9.9));
}

TEST_CASE("net-playout: advance moves render_time forward by dt",
          "[net]") {
    PlayoutClock clk;
    clk.configure(0.5f);  // a real interpolation delay

    clk.on_snapshot(100.0);  // render_time -> 100.0 - 0.5 == 99.5
    CHECK(clk.render_time_s() == Catch::Approx(99.5));

    // Lockstep: a fresh snapshot then a matching dt advance keeps render_time in
    // the healthy band exactly `delay` behind the newest, so it advances cleanly
    // by dt with no re-sync. After: 99.5 + 0.25 == 99.75 == 100.25 - 0.5.
    clk.on_snapshot(100.25);
    clk.advance(0.25f);
    CHECK(clk.render_time_s() == Catch::Approx(99.75));
}

TEST_CASE("net-playout: render_time stays one delay behind a steadily arriving stream",
          "[net]") {
    PlayoutClock clk;
    const f32 delay = 0.1f;
    clk.configure(delay);

    // Server ticks at 20 Hz (50 ms); render advances in two 25 ms frames per tick
    // so the render clock and the stream march in lockstep.
    f64 server_t = 0.0;
    clk.on_snapshot(server_t);  // anchor: render_time = -delay

    for (int tick = 0; tick < 40; ++tick) {
        server_t += 0.05;
        clk.on_snapshot(server_t);
        clk.advance(0.025f);
        clk.advance(0.025f);
    }

    // After a steady run the render clock trails the newest snapshot by ~delay.
    const f64 expected = clk.latest_server_time_s() - static_cast<f64>(delay);
    CHECK(clk.render_time_s() == Catch::Approx(expected).margin(1.0e-6));
}

TEST_CASE("net-playout: starving past the available data re-syncs back behind the newest",
          "[net]") {
    PlayoutClock clk;
    const f32 delay = 0.1f;
    clk.configure(delay);

    clk.on_snapshot(5.0);  // render_time = 4.9, target with delay = 4.9

    // No fresh snapshots arrive, but the renderer keeps advancing well past the
    // newest available data — it would have nothing to interpolate toward.
    clk.advance(0.5f);  // render_time would reach 5.4 (past 5.0)

    // It must snap back to (newest - delay), never lead the stream.
    const f64 target = 5.0 - static_cast<f64>(delay);
    CHECK(clk.render_time_s() == Catch::Approx(target));
    // And it is strictly behind the newest received server time.
    CHECK(clk.render_time_s() < clk.latest_server_time_s());
}

TEST_CASE("net-playout: a large lag behind the stream hard-snaps to the target",
          "[net]") {
    PlayoutClock clk;
    const f32 delay = 0.1f;
    clk.configure(delay);

    clk.on_snapshot(0.0);  // render_time = -0.1
    // A huge jump in server time (latency collapsed / long gap then a burst):
    // the render clock is now seconds behind the target and must hard-snap.
    clk.on_snapshot(100.0);
    clk.advance(0.016f);  // tiny frame; target ~= 99.9, error far below -threshold

    const f64 target = 100.0 - static_cast<f64>(delay);
    CHECK(clk.render_time_s() == Catch::Approx(target));
}

TEST_CASE("net-playout: a small drift behind the stream is eased not snapped",
          "[net]") {
    PlayoutClock clk;
    const f32 delay = 0.1f;
    clk.configure(delay);

    clk.on_snapshot(1.0);  // render_time = 0.9, target = 0.9
    // The stream pulls slightly ahead (40 ms) — inside the hard-resync threshold.
    clk.on_snapshot(1.04);  // target now 0.94, render still 0.9 -> error -0.04
    clk.advance(0.001f);    // tiny frame: render -> 0.901, error -0.039

    const f64 target = 1.04 - static_cast<f64>(delay);  // 0.94
    // EASE: we moved a fraction toward target, so render is now strictly between
    // its pre-advance value and the target — NOT snapped all the way to it.
    CHECK(clk.render_time_s() > 0.9);
    CHECK(clk.render_time_s() < target);
}

TEST_CASE("net-playout: an out-of-order older snapshot is ignored",
          "[net]") {
    PlayoutClock clk;
    clk.configure(0.1f);

    clk.on_snapshot(50.0);
    CHECK(clk.latest_server_time_s() == Catch::Approx(50.0));

    // Older and duplicate timestamps must not move the stream backwards.
    clk.on_snapshot(49.0);
    CHECK(clk.latest_server_time_s() == Catch::Approx(50.0));
    clk.on_snapshot(50.0);
    CHECK(clk.latest_server_time_s() == Catch::Approx(50.0));

    // A strictly-newer one is accepted.
    clk.on_snapshot(50.5);
    CHECK(clk.latest_server_time_s() == Catch::Approx(50.5));
}

TEST_CASE("net-playout: non-finite and non-positive dt are ignored by advance",
          "[net]") {
    PlayoutClock clk;
    clk.configure(0.0f);
    clk.on_snapshot(10.0);  // render_time = 10.0
    clk.on_snapshot(10.5);  // keep stream ahead so a valid advance would not snap

    const f64 before = clk.render_time_s();
    clk.advance(0.0f);                                   // zero
    CHECK(clk.render_time_s() == Catch::Approx(before));
    clk.advance(-0.5f);                                  // negative
    CHECK(clk.render_time_s() == Catch::Approx(before));
    clk.advance(std::numeric_limits<f32>::quiet_NaN());  // NaN
    CHECK(clk.render_time_s() == Catch::Approx(before));
    clk.advance(std::numeric_limits<f32>::infinity());   // Inf
    CHECK(clk.render_time_s() == Catch::Approx(before));
}

TEST_CASE("net-playout: reset forgets the stream but keeps the configured delay",
          "[net]") {
    PlayoutClock clk;
    clk.configure(0.2f);
    clk.on_snapshot(7.0);
    CHECK(clk.has_snapshots());

    clk.reset();
    CHECK_FALSE(clk.has_snapshots());
    CHECK(clk.render_time_s() == Catch::Approx(0.0));
    CHECK(clk.latest_server_time_s() == Catch::Approx(0.0));

    // Delay survives reset(): the next first snapshot still trails by 0.2.
    clk.on_snapshot(20.0);
    CHECK(clk.render_time_s() == Catch::Approx(19.8));
}

TEST_CASE("net-playout: identical config and snapshot plus dt sequences yield identical render_time",
          "[net][determinism]") {
    auto run = []() {
        PlayoutClock clk;
        clk.configure(0.08f);
        f64 server_t = 0.0;
        clk.on_snapshot(server_t);
        f64 dts[] = {0.016, 0.017, 0.015, 0.020, 0.011};
        for (int i = 0; i < 25; ++i) {
            server_t += 0.05;
            clk.on_snapshot(server_t);
            clk.advance(static_cast<f32>(dts[i % 5]));
        }
        return clk.render_time_s();
    };

    const f64 a = run();
    const f64 b = run();
    // Bit-identical: pure f64/f32 algebra, no RNG, no transcendentals.
    CHECK(a == b);
}
