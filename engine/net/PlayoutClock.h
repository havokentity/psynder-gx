// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — client playout (interpolation) clock. Lane 18 (net).
//
// The client receives authoritative snapshots at the server tick rate and feeds
// them into a SnapshotInterpBuffer (SnapshotInterp.h), which interpolates entity
// state between the two snapshots that bracket a requested RENDER TIME. For that
// bracketing to always have a snapshot on EACH side, the render time must trail
// the newest received snapshot by a fixed INTERPOLATION DELAY (one or two server
// ticks plus a jitter margin — see RttEstimator::interp_delay_s). This clock owns
// that render time: it tracks the newest received server time, advances a local
// render clock by frame dt, and reports render_time = (newest_received - delay)
// to hand to SnapshotInterpBuffer::sample().
//
// Why a SEPARATE local clock rather than just newest_received - delay each frame:
// snapshots arrive in lumpy bursts (jitter, coalescing, loss) but rendering must
// be SMOOTH. So the render time is advanced continuously by real frame dt, and
// the discrete stream of newest-received times is used only as a TARGET to gently
// re-sync toward when the local clock drifts — never as the per-frame value.
//
// Re-sync rule (documented; see advance() below):
//   target       = latest_received - delay        (where we WANT to be)
//   error        = render_time - target           (>0 = ahead/starved, <0 = behind)
//   HARD SNAP   when error >  0           (starved: rendered past the newest data,
//                 i.e. there is no future snapshot left to interpolate toward) OR
//               when error < -kHardResyncBehind_s (so far behind the data is about
//                 to be evicted / latency has collapsed): render_time = target.
//   EASE        when -kHardResyncBehind_s <= error <= 0 but |error| is non-trivial:
//                 nudge a fixed fraction (kEaseRate) of the remaining error toward
//                 target each call — render_time -= error * kEaseRate — so small
//                 drifts are absorbed invisibly instead of with a visible jump.
//   HOLD        otherwise (we are comfortably within the interpolation window):
//                 leave render_time exactly as advanced by dt.
// Pure f64/f32 algebra (+,-,*,/ and comparisons) — no transcendentals, no RNG —
// so the same (config, snapshot + dt sequence) yields a bit-identical render_time
// on every platform. Strict-FP net lane (-fno-fast-math -ffp-contract=off,
// DESIGN-PSYNDER-GX.md §14). No per-call heap: the whole state is two doubles, a
// float and a bool.

#pragma once

#include "core/Types.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// PlayoutClock — the client render clock that trails the snapshot stream by a
// fixed interpolation delay and re-syncs when it drifts. See the file header
// for the re-sync rule and determinism contract.
// ──────────────────────────────────────────────────────────────────────────
class PlayoutClock {
public:
    // Default interpolation delay (seconds) before the first configure(): 100 ms,
    // roughly two ticks of a 20 Hz server — a sane, conservative default.
    static constexpr f32 kDefaultDelay_s = 0.1f;

    // Below this much positive error the EASE branch is a no-op (we treat the
    // clock as "on target"); avoids dithering render_time by sub-microsecond
    // amounts every frame.
    static constexpr f64 kEaseDeadband_s = 1.0e-6;

    // How aggressively the EASE branch closes a small lag each call: a fixed
    // fraction of the remaining error. 0.125 (=1/8) matches the gentle EWMA gain
    // used elsewhere in the net lane (RttEstimator::kAlpha) — visually smooth.
    static constexpr f64 kEaseRate = 0.125;

    // If the render clock falls MORE than this far behind the target (latency
    // collapsed, or the data we need is about to be evicted from the interp
    // ring), stop easing and HARD-SNAP straight to the target. 250 ms is well
    // inside the SnapshotInterpBuffer's default history window.
    static constexpr f64 kHardResyncBehind_s = 0.25;

    PlayoutClock() noexcept;

    // Set the fixed delay (seconds) the render clock trails the newest snapshot
    // by. Non-finite or negative values are ignored (delay left unchanged); the
    // already-running render_time is NOT retroactively shifted — the new delay
    // takes effect through the re-sync target on the next advance().
    void configure(f32 interp_delay_s) noexcept;

    // Forget the stream: has_snapshots() is false again, render_time is 0, and
    // the next on_snapshot() re-initialises render_time. Delay is retained.
    void reset() noexcept;

    // Record the newest received server time (seconds). Monotonic: a time at or
    // before the latest already seen is IGNORED (an out-of-order / duplicate /
    // older packet must not drag the stream backwards). The FIRST accepted
    // snapshot initialises render_time = server_time - delay so the clock starts
    // exactly one delay behind the freshest data.
    void on_snapshot(f64 server_time_s) noexcept;

    // Advance the local render clock by frame dt (seconds), then re-sync per the
    // rule in the file header. Non-finite or non-positive dt is ignored (no
    // advance, no re-sync) — a paused/clamped frame must not corrupt the clock.
    void advance(f32 dt_s) noexcept;

    // The time to sample the interpolation buffer at: the local render clock.
    // 0 before the first snapshot has arrived.
    f64 render_time_s() const noexcept { return render_time_; }

    // Has at least one snapshot been recorded since construction/reset?
    bool has_snapshots() const noexcept { return has_; }

    // The newest server time recorded so far (seconds). 0 before any snapshot.
    f64 latest_server_time_s() const noexcept { return latest_received_; }

private:
    f64  latest_received_ = 0.0;             // newest server time seen
    f64  render_time_     = 0.0;             // local render clock (what we sample at)
    f32  delay_           = kDefaultDelay_s; // fixed trail behind the newest snapshot
    bool has_             = false;           // any snapshot recorded yet?
};

}  // namespace psynder::net
