// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — client-side snapshot interpolation buffer. Lane 18 (net).
//
// A client receives authoritative snapshots at the server tick rate (e.g. 20-64
// Hz) but renders at a much higher frame rate. To hide that rate mismatch — and
// to absorb jitter in packet arrival — the client renders the world slightly in
// the PAST (an "interpolation delay", typically one or two server ticks) and,
// for each rendered frame, LERPs every entity's state between the two buffered
// snapshots that bracket the chosen render time. The result is smooth motion
// that lags reality by a small, bounded amount.
//
// This layer is COSMETIC / client-side render smoothing — it is NOT the
// authoritative simulation and never feeds back into it. It still MUST be pure
// and deterministic: given the same pushed snapshots and the same render time it
// produces bit-identical output. Strict-FP net lane (-fno-fast-math
// -ffp-contract=off, DESIGN-PSYNDER-GX.md §14); every operation here is +,-,*,/
// only — NO transcendentals (no sin/cos/atan2), so no platform libm drift.
//
// Clamp / no-extrapolation policy (the standard safe behaviour): a render time
// BEFORE the oldest buffered snapshot outputs the oldest snapshot verbatim; a
// render time AFTER the newest outputs the newest verbatim. We never extrapolate
// past either endpoint — extrapolation amplifies one bad packet into visible
// overshoot/rubber-banding, so the safe degenerate is to hold the endpoint.
//
// Shortest-arc yaw: yaw is an angle, so a naive component LERP would sweep the
// long way around the +/-180 wrap (e.g. 170 -> -170 would pass through 0 instead
// of through 180). `lerp_yaw_deg` interpolates along the SHORTER arc using pure
// algebra (wrap the difference into [-180,180], step along it, wrap again).

#pragma once

#include "core/Types.h"
#include "net/SnapshotReplication.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// wrap180 — fold an angle in degrees into the half-open-ish range [-180, 180]
// by adding/subtracting whole turns of 360. Pure algebra: no fmod, no trig.
// For inputs already in a sane neighbourhood of the range (the only case the
// interpolator produces — a difference of two angles each in [-180,180], plus a
// fraction of it) a couple of fixed correction steps suffice, but we loop to
// stay correct for any finite input. Determinism: branches + add/sub only.
// ──────────────────────────────────────────────────────────────────────────
inline f32 wrap180(f32 deg) noexcept {
    // Bring large magnitudes down with whole turns; the loops run a bounded
    // number of times for any realistic angle and never invoke a transcendental.
    while (deg > 180.f) deg -= 360.f;
    while (deg < -180.f) deg += 360.f;
    return deg;
}

// ──────────────────────────────────────────────────────────────────────────
// lerp_yaw_deg — shortest-arc interpolation of two yaw angles (degrees) at
// fraction `t`. Both endpoints are treated as angles, so we interpolate along
// the SHORTER of the two arcs between them:
//   d      = wrap180(b - a)   — signed shortest delta in [-180, 180]
//   result = wrap180(a + t*d) — step a fraction of that delta, re-wrapped
// Pure algebra (+,-,*,/ and the wrap above) — NO sin/cos/atan2. `t` is used as
// given; the buffer's sampler clamps it to [0,1] before calling.
// Example: a=170, b=-170, t=0.5 -> d = wrap180(-340) = 20, result =
// wrap180(170 + 10) = 180 (the SHORT way through +/-180, not through 0).
// ──────────────────────────────────────────────────────────────────────────
f32 lerp_yaw_deg(f32 a_deg, f32 b_deg, f32 t) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// SnapshotInterpBuffer — a fixed-capacity ring of timestamped snapshots plus
// the sampler that interpolates between them at a requested render time.
//
// push(): store a snapshot stamped at `server_time_s`. Snapshots are assumed
// pushed in NON-DECREASING time order. A push whose timestamp is STRICTLY OLDER
// than the newest buffered snapshot is ignored (it would break the time-ordered
// ring the sampler relies on); a push whose timestamp EQUALS the newest replaces
// that newest entry in place (a late correction to the same tick). Once the ring
// is full the oldest snapshot is evicted to make room.
//
// sample(): pick the two snapshots A,B with A.time <= render_time <= B.time,
// compute t = (render_time - A.time)/(B.time - A.time) (t=0 for a zero-length
// interval, t clamped to [0,1]), and for every entity id present in BOTH A and B
// emit the interpolated state (position component LERP, yaw shortest-arc LERP).
// An id present in only ONE of the bracketing pair is emitted at its last known
// value — no extrapolation. Outside the buffered time span the nearest endpoint
// snapshot is emitted verbatim (see the clamp policy at the top of this file).
//
// Determinism / no-growth: `out` is cleared then filled in ASCENDING id order.
// The sampler does a single-pass id-merge straight into the caller-owned `out`,
// so the steady-state path grows no heap beyond `out` itself (whose capacity the
// caller reuses across frames). push() reuses evicted/replaced snapshot vectors
// in place to avoid per-tick reallocation. All math is +,-,*,/.
// ──────────────────────────────────────────────────────────────────────────
class SnapshotInterpBuffer {
public:
    // Default capacity: 16 snapshots. At a 64 Hz server tick that is a quarter
    // second of history — comfortably more than any sane interpolation delay,
    // and enough slack to ride out a burst of late/lost packets.
    static constexpr usize kDefaultCapacity = 16;

    SnapshotInterpBuffer() : SnapshotInterpBuffer(kDefaultCapacity) {}

    // `capacity` is clamped up to 1 — a zero-capacity ring cannot hold anything
    // and is never useful.
    explicit SnapshotInterpBuffer(usize capacity);

    // Store `states` stamped at `server_time_s`. See the class comment for the
    // non-decreasing-time / duplicate-replace / oldest-evict policy. Copies the
    // span into an internal snapshot (the caller's storage need not outlive the
    // call).
    void push(f64 server_time_s, std::span<const EntityState> states);

    // Interpolate the buffered snapshots at `render_time_s` into `out`. Returns
    // false (and clears `out`) when the buffer is empty; otherwise clears `out`
    // and fills it in ascending id order, returning true. See the class comment
    // and the file header for the bracket/clamp/no-extrapolation rules.
    bool sample(f64 render_time_s, std::vector<EntityState>& out) const;

    // Drop every buffered snapshot. Capacity is retained.
    void clear() noexcept;

    // Number of buffered snapshots currently held.
    usize size() const noexcept { return snaps_.size(); }

    // True when no snapshots are buffered.
    bool empty() const noexcept { return snaps_.empty(); }

    // The configured maximum number of buffered snapshots.
    usize capacity() const noexcept { return capacity_; }

private:
    // One buffered snapshot: a server timestamp plus the entity set as of that
    // tick. `states` is kept sorted ASCENDING by id on insertion so the sampler
    // can merge two snapshots by a single linear id-merge (no per-sample sort).
    struct Snapshot {
        f64                      time = 0.0;
        std::vector<EntityState> states;
    };

    // Merge the two id-sorted snapshots A,B at fraction `t` into `out` (already
    // cleared by the caller): shared ids interpolated, single-side ids passed
    // through at their known value, output ascending by id.
    static void interp_into(const Snapshot& a, const Snapshot& b, f32 t,
                            std::vector<EntityState>& out);

    // Copy `out` from a single snapshot verbatim (the empty-bracket / clamp and
    // exact-hit paths). `out` is assumed already cleared.
    static void copy_into(const Snapshot& s, std::vector<EntityState>& out);

    usize                 capacity_ = kDefaultCapacity;
    std::vector<Snapshot> snaps_;   // oldest at front, newest at back; time-sorted
};

}  // namespace psynder::net
