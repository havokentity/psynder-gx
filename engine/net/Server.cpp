// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — Server scheduler impl. Lane 18 (net).
//
// Tick loop:
//   1. Sleep until `next_deadline_` (sleep_until + short final spin).
//   2. Fire the tick callback.
//   3. Measure overrun; advance `next_deadline_`.
//
// Drift correction rule (per task spec): we never run "catch-up" ticks. If a
// tick took longer than the frame period, we drop the missed time on the
// floor — the next deadline anchors to `now + frame_period`, not to the
// stale `last_deadline + frame_period`. This is the standard
// "fixed-timestep with no accumulator" policy. Game state stays
// deterministic because the callback always receives `cfg_.frame_sec` as
// its dt parameter regardless of measured drift.
//
// Time source: std::chrono::steady_clock. This matches engine/platform/
// Platform.cpp's `Clock::ticks_now()` (literally
// `steady_clock::now().time_since_epoch().count()`); we inline that here to
// keep psynder_net's link surface independent of the platform aggregator
// (which is orchestrator-owned and not in this lane's CMake scope).

#include "Server.h"

#include <chrono>
#include <thread>

namespace psynder::net {

namespace {

using SteadyClock  = std::chrono::steady_clock;
using SteadyPoint  = SteadyClock::time_point;
using SteadyDur    = SteadyClock::duration;

SteadyPoint now() noexcept { return SteadyClock::now(); }

SteadyDur frame_period_from_cfg(const TickConfig& cfg) noexcept {
    // Nanoseconds-resolution period. tick_hz is integer, so this is exact
    // for 64/128 (1e9 / 64 = 15625000ns, 1e9 / 128 = 7812500ns).
    const u64 ns = 1'000'000'000ull / static_cast<u64>(cfg.tick_hz);
    return std::chrono::nanoseconds(static_cast<i64>(ns));
}

// Coarse sleep, then a short spin to land on the deadline within ~10 us on a
// quiet machine. OS scheduler granularity is the bottleneck: ~1 ms on Linux
// SCHED_OTHER and Apple Silicon, ~15 ms on Windows without timeBeginPeriod.
// We sleep until 250 us before the deadline, then spin.
void high_res_sleep_until(SteadyPoint deadline, bool spin_final) noexcept {
    constexpr auto kSpinBudget = std::chrono::microseconds(250);
    const SteadyPoint sleep_until_pt =
        spin_final ? deadline - kSpinBudget : deadline;
    const SteadyPoint t0 = now();
    if (sleep_until_pt > t0) {
        std::this_thread::sleep_until(sleep_until_pt);
    }
    // Final busy-wait. Yield-free: on the last few us we want minimum wake
    // latency (no scheduler hop), so we plain-loop.
    while (now() < deadline) {
        // tight spin
    }
}

}  // namespace

Server::~Server() noexcept { stop(); }

bool Server::start(const TickConfig& cfg, u16 bind_port) noexcept {
    if (is_running()) return false;
    cfg_           = cfg;
    current_tick_  = 0;
    stop_requested_.store(false);

    const auto period_dur = frame_period_from_cfg(cfg);
    frame_period_ = static_cast<u64>(period_dur.count());
    if (frame_period_ == 0) return false;

    if (!socket_.open(bind_port)) return false;

    const SteadyPoint anchor = now() + period_dur;
    next_deadline_ = static_cast<u64>(anchor.time_since_epoch().count());
    return true;
}

void Server::stop() noexcept {
    if (!is_running()) return;
    socket_.close();
    stop_requested_.store(false);
}

u64 Server::run(u32 tick_count) noexcept {
    if (!is_running()) return 0;
    u64 executed = 0;
    for (u32 i = 0; i < tick_count; ++i) {
        const SteadyPoint dl{SteadyDur{static_cast<i64>(next_deadline_)}};
        high_res_sleep_until(dl, spin_lock_final_);
        const SteadyPoint actual = now();
        const u64 actual_count = static_cast<u64>(actual.time_since_epoch().count());
        if (!step_one_tick_(actual_count)) break;
        ++executed;
    }
    return executed;
}

void Server::run_until_stop() noexcept {
    if (!is_running()) return;
    while (!stop_requested_.load()) {
        const SteadyPoint dl{SteadyDur{static_cast<i64>(next_deadline_)}};
        high_res_sleep_until(dl, spin_lock_final_);
        const SteadyPoint actual = now();
        const u64 actual_count = static_cast<u64>(actual.time_since_epoch().count());
        if (!step_one_tick_(actual_count)) break;
    }
}

bool Server::step_one_tick_(u64 deadline_now) noexcept {
    const u64 tick_started_at = deadline_now;
    ++current_tick_;

    if (tick_cb_) {
        tick_cb_(current_tick_, cfg_.frame_sec);
    }

    const u64 tick_ended_at =
        static_cast<u64>(now().time_since_epoch().count());
    const u64 tick_cost = tick_ended_at - tick_started_at;
    stats_.ticks_executed.fetch_add(1);

    // Schedule next deadline. If the tick overran the frame period we drop
    // the missed time on the floor and re-anchor to `ended + period`
    // rather than letting `next_deadline_ += period` race ahead of `now`.
    if (tick_cost > frame_period_) {
        const u64 overrun = tick_cost - frame_period_;
        stats_.ticks_overran.fetch_add(1);
        stats_.total_drift_ns.fetch_add(overrun);
        u64 prev_max = stats_.max_overrun_ns.load();
        while (overrun > prev_max
               && !stats_.max_overrun_ns.compare_exchange_weak(prev_max, overrun)) {
            // retry
        }
        next_deadline_ = tick_ended_at + frame_period_;
    } else {
        next_deadline_ += frame_period_;
        // Safety: if next_deadline_ has fallen behind `now` (e.g. coarse
        // OS sleep on a busy machine across multiple ticks), re-anchor.
        if (next_deadline_ < tick_ended_at) {
            next_deadline_ = tick_ended_at + frame_period_;
        }
    }

    if (stop_requested_.load()) return false;
    return true;
}

}  // namespace psynder::net
