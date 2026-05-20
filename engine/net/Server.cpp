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

#if defined(_WIN32)
// timeBeginPeriod/timeEndPeriod raise the system timer resolution so
// std::this_thread::sleep_until in high_res_sleep_until() lands within ~1 ms of
// the deadline. Windows' default ~15.6 ms granularity otherwise overshoots every
// 128 Hz frame (the 250 us final spin can't recover 15 ms), dropping the server
// to ~70 Hz. WIN32_LEAN_AND_MEAN + NOMINMAX keep <windows.h> from polluting the
// TU; winmm provides timeBeginPeriod (linked via the pragma so no CMake change).
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <timeapi.h>
#  pragma comment(lib, "winmm")
#endif

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

    frame_period_ = frame_period_from_cfg(cfg);
    if (frame_period_ == SteadyDur::zero()) return false;

    if (!socket_.open(bind_port)) return false;

#if defined(_WIN32)
    // Raise timer resolution to 1 ms for the lifetime of the run loop so the
    // 128-tick deadline waits are accurate (see the include-block note + the
    // granularity comment in high_res_sleep_until). Released in stop().
    if (timeBeginPeriod(1) == TIMERR_NOERROR) {
        win_timer_period_set_ = true;
    }
#endif

    next_deadline_ = now() + frame_period_;
    return true;
}

void Server::stop() noexcept {
    if (!is_running()) return;
    socket_.close();
    stop_requested_.store(false);
#if defined(_WIN32)
    if (win_timer_period_set_) {
        timeEndPeriod(1);
        win_timer_period_set_ = false;
    }
#endif
}

u64 Server::run(u32 tick_count) noexcept {
    if (!is_running()) return 0;
    u64 executed = 0;
    for (u32 i = 0; i < tick_count; ++i) {
        high_res_sleep_until(next_deadline_, spin_lock_final_);
        if (!step_one_tick_(now())) break;
        ++executed;
    }
    return executed;
}

void Server::run_until_stop() noexcept {
    if (!is_running()) return;
    while (!stop_requested_.load()) {
        high_res_sleep_until(next_deadline_, spin_lock_final_);
        if (!step_one_tick_(now())) break;
    }
}

bool Server::step_one_tick_(SteadyPoint actual_start) noexcept {
    ++current_tick_;

    if (tick_cb_) {
        tick_cb_(current_tick_, cfg_.frame_sec);
    }

    const SteadyPoint tick_ended_at = now();
    const SteadyDur   tick_cost     = tick_ended_at - actual_start;
    stats_.ticks_executed.fetch_add(1);

    // Schedule next deadline. If the tick overran the frame period we drop
    // the missed time on the floor and re-anchor to `ended + period`
    // rather than letting `next_deadline_ += period` race ahead of `now`.
    if (tick_cost > frame_period_) {
        const SteadyDur overrun = tick_cost - frame_period_;
        const u64 overrun_ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(overrun).count());
        stats_.ticks_overran.fetch_add(1);
        stats_.total_drift_ns.fetch_add(overrun_ns);
        u64 prev_max = stats_.max_overrun_ns.load();
        while (overrun_ns > prev_max
               && !stats_.max_overrun_ns.compare_exchange_weak(prev_max, overrun_ns)) {
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
