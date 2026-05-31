// SPDX-License-Identifier: MIT
//
// tests/unit/net_loss_model.cpp — the deterministic loss / reorder model.

#include "net/LossModel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::net;

TEST_CASE("loss model: rate zero drops nothing and rate one drops all", "[net]") {
    LossConfig none{0.0f, 0.0f, 0u, 42u};
    LossConfig all{1.0f, 0.0f, 0u, 42u};
    CHECK(measured_loss(none, 0, 5000) == Catch::Approx(0.0f));
    CHECK(measured_loss(all, 0, 5000) == Catch::Approx(1.0f));
    for (u64 s = 0; s < 100; ++s) {
        CHECK_FALSE(drops(none, s));
        CHECK(drops(all, s));
    }
}

TEST_CASE("loss model: measured loss tracks the configured rate", "[net]") {
    LossConfig cfg{0.30f, 0.0f, 0u, 12345u};
    const f32 measured = measured_loss(cfg, 0, 10000);
    CHECK(measured == Catch::Approx(0.30f).margin(0.03f));  // well-distributed hash
}

TEST_CASE("loss model: the same seq and config always decide the same", "[net]") {
    LossConfig cfg{0.4f, 0.5f, 6u, 999u};
    for (u64 s = 0; s < 64; ++s) {
        CHECK(drops(cfg, s) == drops(cfg, s));
        CHECK(extra_delay_ticks(cfg, s) == extra_delay_ticks(cfg, s));
    }
}

TEST_CASE("loss model: a different seed yields a different pattern same rate",
          "[net]") {
    LossConfig a{0.3f, 0.0f, 0u, 1u};
    LossConfig b{0.3f, 0.0f, 0u, 2u};
    u32 differ = 0u;
    for (u64 s = 0; s < 2000; ++s) {
        if (drops(a, s) != drops(b, s)) ++differ;
    }
    CHECK(differ > 0u);  // the two seeds disagree on many packets
    // ...but both approximate the same rate.
    CHECK(measured_loss(a, 0, 8000) == Catch::Approx(0.3f).margin(0.03f));
    CHECK(measured_loss(b, 0, 8000) == Catch::Approx(0.3f).margin(0.03f));
}

TEST_CASE("loss model: reorder delay is zero off, in range when it fires",
          "[net]") {
    LossConfig noreorder{0.0f, 0.0f, 8u, 7u};
    for (u64 s = 0; s < 200; ++s) CHECK(extra_delay_ticks(noreorder, s) == 0u);

    LossConfig cfg{0.0f, 0.5f, 8u, 7u};
    u32 fired = 0u;
    for (u64 s = 0; s < 4000; ++s) {
        const u32 d = extra_delay_ticks(cfg, s);
        if (d != 0u) {
            ++fired;
            CHECK(d >= 1u);
            CHECK(d <= 8u);
        }
    }
    CHECK(fired > 0u);
    // ~50% of packets reorder.
    CHECK(static_cast<f32>(fired) / 4000.0f == Catch::Approx(0.5f).margin(0.04f));
}

TEST_CASE("loss model: loss and reorder are independent streams", "[net]") {
    // A heavy-loss + heavy-reorder config: some packets are BOTH dropped AND
    // carry a delay (the streams are uncorrelated, not the same coin).
    LossConfig cfg{0.6f, 0.6f, 5u, 555u};
    u32 dropped_and_delayed = 0u;
    for (u64 s = 0; s < 4000; ++s) {
        if (drops(cfg, s) && extra_delay_ticks(cfg, s) != 0u) ++dropped_and_delayed;
    }
    CHECK(dropped_and_delayed > 0u);  // independence => the overlap is non-empty
}

TEST_CASE("loss model: identical configs decide identically across a sweep",
          "[net][determinism]") {
    LossConfig a{0.35f, 0.4f, 7u, 0xDEADBEEFu};
    LossConfig b{0.35f, 0.4f, 7u, 0xDEADBEEFu};
    for (u64 s = 0; s < 3000; ++s) {
        CHECK(drops(a, s) == drops(b, s));
        CHECK(extra_delay_ticks(a, s) == extra_delay_ticks(b, s));
    }
}
