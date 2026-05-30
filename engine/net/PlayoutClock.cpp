// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — client playout (interpolation) clock. Lane 18 (net).
//
// See PlayoutClock.h for the full contract and the documented re-sync rule.
// Strict-FP net lane: pure +,-,*,/ and comparisons (the only library calls are
// std::isfinite guards) — no transcendentals, no RNG, no per-call heap.

#include "net/PlayoutClock.h"

#include <cmath>

namespace psynder::net {

PlayoutClock::PlayoutClock() noexcept = default;

void PlayoutClock::configure(f32 interp_delay_s) noexcept {
    // Ignore garbage (NaN/Inf) and negative delays — a delay must be >= 0 to make
    // the render clock trail (not lead) the snapshot stream.
    if (!std::isfinite(interp_delay_s) || interp_delay_s < 0.0f) return;
    delay_ = interp_delay_s;
}

void PlayoutClock::reset() noexcept {
    latest_received_ = 0.0;
    render_time_     = 0.0;
    has_             = false;
    // delay_ is intentionally retained across reset().
}

void PlayoutClock::on_snapshot(f64 server_time_s) noexcept {
    // Ignore non-finite timestamps outright.
    if (!std::isfinite(server_time_s)) return;

    if (!has_) {
        // First accepted snapshot: anchor both clocks. render_time starts exactly
        // one delay behind the freshest data so the interp buffer always brackets.
        latest_received_ = server_time_s;
        render_time_     = server_time_s - static_cast<f64>(delay_);
        has_             = true;
        return;
    }

    // Monotonic stream: only accept times STRICTLY newer than what we have. An
    // out-of-order / duplicate / older packet must not drag the target backwards.
    if (server_time_s > latest_received_) {
        latest_received_ = server_time_s;
    }
}

void PlayoutClock::advance(f32 dt_s) noexcept {
    // Guard non-finite / non-positive dt — a paused or clamped frame must neither
    // advance nor re-sync the clock.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) return;

    // Advance the render clock by real frame time first (the smooth path).
    render_time_ += static_cast<f64>(dt_s);

    // Nothing to re-sync against until a stream exists.
    if (!has_) return;

    // target = where we WANT to be: one delay behind the newest received time.
    const f64 target = latest_received_ - static_cast<f64>(delay_);
    const f64 error  = render_time_ - target;  // >0 ahead/starved, <0 behind

    if (error > 0.0) {
        // STARVED: we have rendered at or past the newest snapshot — there is no
        // future snapshot left to interpolate toward. Hard-snap back onto target.
        render_time_ = target;
        return;
    }

    if (error < -kHardResyncBehind_s) {
        // Too far behind (latency collapsed, or the data we still want is about to
        // be evicted): hard-snap straight to target rather than crawl via easing.
        render_time_ = target;
        return;
    }

    // Mildly behind: ease a fixed fraction of the remaining lag toward target so
    // the correction is invisible. error is in (-kHardResyncBehind_s, 0], so this
    // moves render_time FORWARD (render_time -= error*rate, error < 0). The tiny
    // deadband around 0 keeps us from dithering when essentially on target.
    if (error < -kEaseDeadband_s) {
        render_time_ -= error * kEaseRate;
    }
}

}  // namespace psynder::net
