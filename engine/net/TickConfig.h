// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — server tick-rate configuration. Lane 18 (net).
//
// DESIGN-PSYNDER-GX.md §10.4:
//   "Tick rate: 64-tick default for casual, 128-tick competitive.
//    Configurable per server."
//
// A TickConfig value is selected once at server startup and kept constant for
// the lifetime of the match. The frame interval in seconds is exactly
// `1.0 / tick_hz`. Both 64 and 128 Hz are first-class supported rates; the
// struct also allows other power-of-two values for testing.
//
// Determinism note: TickConfig fields feed into the lag-comp rewind window
// (LagComp.h) and the demo format header (DemoFormat.h). Any change in tick
// rate invalidates existing .psydem files — the demo reader checks the stored
// tick rate against the binary and refuses a mismatch.
//
// Compile flag: net TUs that include this header MUST be compiled with
//   -fno-fast-math -ffp-contract=off
// (DESIGN-PSYNDER-GX.md §14, lockstep determinism).

#pragma once

#include "core/Types.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────────
// Supported tick rates.
// Add new values here only; removing existing values breaks .psydem backwards-
// compat.
// ──────────────────────────────────────────────────────────────────────────────
enum class TickRate : u8 {
    Hz64  = 64,   // Default — casual / community servers.
    Hz128 = 128,  // Competitive — tournament / ranked servers.
};

// ──────────────────────────────────────────────────────────────────────────────
// TickConfig — immutable once the server starts a match.
// ──────────────────────────────────────────────────────────────────────────────
struct TickConfig {
    TickRate tick_rate    = TickRate::Hz64;

    // Derived values — computed by make_tick_config(); never set by hand.
    u32      tick_hz      = 64;    // ticks per second (same as tick_rate cast to u32)
    f64      frame_sec    = 1.0 / 64.0;  // seconds per tick
    f64      frame_ms     = 1000.0 / 64.0;  // milliseconds per tick
    u32      lag_comp_ticks = 13;  // ceil(200 ms / frame_ms) — default rewind depth

    // Number of bits in the sub-tick fraction field per DESIGN §10.4.
    // "16-bit fraction-of-tick aim sample." Fixed — not rate-dependent.
    static constexpr u8 kSubTickFractionBits = 16;

    // Absolute maximum lag-comp window (ms). Server enforces this ceiling even
    // if the configured rewind window is larger.
    static constexpr u32 kMaxLagCompWindowMs = 200u;
};

// Factory — produces a fully-derived TickConfig for the requested rate.
// Use instead of aggregate-initialising TickConfig directly.
inline TickConfig make_tick_config(TickRate rate) noexcept {
    TickConfig cfg;
    cfg.tick_rate     = rate;
    cfg.tick_hz       = static_cast<u32>(rate);
    cfg.frame_sec     = 1.0 / static_cast<f64>(cfg.tick_hz);
    cfg.frame_ms      = 1000.0 / static_cast<f64>(cfg.tick_hz);
    // Ceiling division: how many whole ticks fit in 200 ms?
    cfg.lag_comp_ticks =
        (TickConfig::kMaxLagCompWindowMs + static_cast<u32>(cfg.frame_ms) - 1u)
        / static_cast<u32>(cfg.frame_ms);
    return cfg;
}

// Convenience factories.
inline TickConfig tick_config_64()  noexcept { return make_tick_config(TickRate::Hz64);  }
inline TickConfig tick_config_128() noexcept { return make_tick_config(TickRate::Hz128); }

}  // namespace psynder::net
