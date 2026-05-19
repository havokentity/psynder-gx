// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — server-authoritative tick scheduler. Lane 18 (net).
//
// DESIGN-PSYNDER-GX.md §10.4:
//   "Tick rate: 64-tick default for casual, 128-tick competitive. Configurable
//    per server."
//
// The Server owns the main tick loop: it sleeps until the next scheduled
// deadline, fires the user-provided per-tick callback, processes inbound
// UDP traffic, and emits outbound snapshots/acks. The loop runs on a
// caller-controlled thread (call run() from your dedicated-server main()).
//
// Drift correction (per task spec):
//   * If a tick took LONGER than 1/tick_hz, we log a warning, advance
//     `current_tick_` by exactly one, and re-anchor the next deadline to
//     `now + frame_period`. We do NOT try to catch up by running multiple
//     ticks back-to-back — that would starve the network thread and
//     amplify the lag spike.
//   * If a tick finished EARLY (the normal case), we high-resolution-wait
//     on sleep_until() with a short final spin to drive jitter under 1 ms
//     on a quiet machine.
//
// Compile flag: -fno-fast-math -ffp-contract=off (DESIGN §14, lockstep
// determinism). All float math here is deterministic IEEE-754; we use it
// only for the tick-budget warning log, not for any state-affecting
// computation.

#pragma once

#include "TickConfig.h"
#include "UdpSocket.h"
#include "core/Types.h"

#include <atomic>
#include <functional>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// TickCallback — invoked once per tick on the server thread. The dt
// argument is the nominal tick period (1/tick_hz in seconds), NOT the
// actual measured interval — we feed nominal so game logic stays
// frame-rate-independent and deterministic.
// ──────────────────────────────────────────────────────────────────────────
using TickCallback = std::function<void(u32 tick, f64 dt_sec)>;

// ──────────────────────────────────────────────────────────────────────────
// ServerStats — accumulated since start. Read on the same thread that calls
// run(); the atomic fields are safe to sample from any thread for live
// telemetry / a health endpoint.
// ──────────────────────────────────────────────────────────────────────────
struct ServerStats {
    std::atomic<u64> ticks_executed{0};
    std::atomic<u64> ticks_overran{0};   // tick took longer than frame_period
    std::atomic<u64> max_overrun_ns{0};  // largest single overrun observed
    std::atomic<u64> total_drift_ns{0};  // cumulative overrun across all ticks
};

// ──────────────────────────────────────────────────────────────────────────
// Server — owns a UdpSocket + a tick scheduler.
//
// Construct with a TickConfig. Bind a port, set the per-tick callback,
// then call run(N) to drive N ticks (or run_until_stop() for an infinite
// loop ended by stop()).
// ──────────────────────────────────────────────────────────────────────────
class Server {
public:
    Server() noexcept = default;
    ~Server() noexcept;

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // Configure tick rate + bind UDP. Returns false on socket failure.
    // `bind_port == 0` lets the OS pick (test mode); read it back via
    // local_port().
    bool start(const TickConfig& cfg, u16 bind_port) noexcept;

    // Tear down the socket. Safe to call repeatedly.
    void stop() noexcept;

    bool is_running() const noexcept { return socket_.is_open(); }

    // Install the per-tick callback. Replace at any time between ticks.
    void set_tick_callback(TickCallback cb) noexcept { tick_cb_ = std::move(cb); }

    // Run exactly `tick_count` ticks then return. Used by tests + the
    // dedicated-server main loop for warmup. Returns the actual number of
    // ticks executed.
    u64 run(u32 tick_count) noexcept;

    // Run until stop() is called from another thread (or from inside the
    // tick callback). Sets a flag the loop checks each iteration.
    void run_until_stop() noexcept;

    // Signal run_until_stop() to exit after the current tick completes.
    void request_stop() noexcept { stop_requested_.store(true); }

    // Introspection.
    u32              current_tick() const noexcept { return current_tick_; }
    const TickConfig& tick_config() const noexcept { return cfg_; }
    u16              local_port()   const noexcept { return socket_.local_port(); }
    const ServerStats& stats()      const noexcept { return stats_; }

    // Test hook: tell the scheduler whether to use a hard spin for the
    // final sub-millisecond wait. Default ON for accuracy under load; tests
    // that just want to verify tick count can flip this OFF to save CPU.
    void set_spin_lock_final_wait(bool on) noexcept { spin_lock_final_ = on; }

private:
    // The actual tick step. Returns false if the user requested a stop
    // inside the callback (so run_until_stop sees it without re-checking).
    bool step_one_tick_(u64 now_ticks) noexcept;

    TickConfig         cfg_{};
    UdpSocket          socket_;
    TickCallback       tick_cb_;
    u32                current_tick_   = 0;
    u64                next_deadline_  = 0;  // platform::Clock::ticks_now units
    u64                frame_period_   = 0;  // in platform clock ticks
    std::atomic<bool>  stop_requested_{false};
    bool               spin_lock_final_ = true;
    ServerStats        stats_;
};

}  // namespace psynder::net
