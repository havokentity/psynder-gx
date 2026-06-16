// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — client-side snapshot interpolation buffer impl. Lane 18 (net).
// See SnapshotInterp.h for the clamp/no-extrapolation policy, the shortest-arc
// yaw rule, and the determinism (strict-FP, +,-,*,/ only) contract.

#include "net/SnapshotInterp.h"

#include <algorithm>

namespace psynder::net {

namespace {

// Component LERP — a + t*(b - a). Pure algebra; matches the position rule in
// the header. Kept tiny + local so the sampler stays branch-light.
inline f32 lerp_f32(f32 a, f32 b, f32 t) noexcept {
    return a + t * (b - a);
}

// Order EntityState ascending by id — the canonical sort key for both the
// per-snapshot store and the sampler's merged output.
inline bool by_id(const EntityState& a, const EntityState& b) noexcept {
    return a.id < b.id;
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────────
// lerp_yaw_deg — see header. d = wrap180(b - a); result = wrap180(a + t*d).
// ──────────────────────────────────────────────────────────────────────────
f32 lerp_yaw_deg(f32 a_deg, f32 b_deg, f32 t) noexcept {
    const f32 d = wrap180(b_deg - a_deg);  // signed shortest delta in [-180,180]
    return wrap180(a_deg + t * d);         // step the fraction, re-wrap
}

// ──────────────────────────────────────────────────────────────────────────
// ctor — clamp capacity up to at least 1 and reserve the ring.
// ──────────────────────────────────────────────────────────────────────────
SnapshotInterpBuffer::SnapshotInterpBuffer(usize capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {
    snaps_.reserve(capacity_);
}

// ──────────────────────────────────────────────────────────────────────────
// push — store `states` at `server_time_s`, honouring the time-ordering /
// duplicate-replace / oldest-evict policy from the header.
// ──────────────────────────────────────────────────────────────────────────
void SnapshotInterpBuffer::push(f64 server_time_s,
                                std::span<const EntityState> states) {
    if (!snaps_.empty()) {
        const f64 newest = snaps_.back().time;
        if (server_time_s < newest) {
            // Strictly out of order — ignore. Accepting it would corrupt the
            // time-sorted ring the sampler's bracket search relies on.
            return;
        }
        if (server_time_s == newest) {
            // Same tick re-pushed — replace the newest entry in place (a late
            // correction). Reuse its vector storage; just refill it sorted.
            Snapshot& top = snaps_.back();
            top.states.assign(states.begin(), states.end());
            std::sort(top.states.begin(), top.states.end(), by_id);
            return;
        }
    }

    // A new, strictly-newer snapshot. Evict the oldest if the ring is full.
    if (snaps_.size() >= capacity_) {
        // Rotate the front (oldest) slot to the back and overwrite it, so we
        // reuse its already-allocated states vector instead of freeing+allocing.
        std::rotate(snaps_.begin(), snaps_.begin() + 1, snaps_.end());
        Snapshot& slot = snaps_.back();
        slot.time = server_time_s;
        slot.states.assign(states.begin(), states.end());
        std::sort(slot.states.begin(), slot.states.end(), by_id);
        return;
    }

    Snapshot snap;
    snap.time = server_time_s;
    snap.states.assign(states.begin(), states.end());
    std::sort(snap.states.begin(), snap.states.end(), by_id);
    snaps_.push_back(std::move(snap));
}

// ──────────────────────────────────────────────────────────────────────────
// copy_into — emit a single snapshot verbatim (already id-sorted on insert).
// ──────────────────────────────────────────────────────────────────────────
void SnapshotInterpBuffer::copy_into(const Snapshot& s,
                                     std::vector<EntityState>& out) {
    out.insert(out.end(), s.states.begin(), s.states.end());
}

// ──────────────────────────────────────────────────────────────────────────
// interp_into — linear id-merge of two id-sorted snapshots at fraction `t`.
// Shared ids interpolate (pos LERP + shortest-arc yaw); single-side ids pass
// through at their known value (no extrapolation). Output stays ascending by id.
// ──────────────────────────────────────────────────────────────────────────
void SnapshotInterpBuffer::interp_into(const Snapshot& a, const Snapshot& b,
                                       f32 t, std::vector<EntityState>& out) {
    const std::vector<EntityState>& va = a.states;
    const std::vector<EntityState>& vb = b.states;

    usize i = 0;  // cursor into A
    usize j = 0;  // cursor into B
    while (i < va.size() && j < vb.size()) {
        const EntityState& ea = va[i];
        const EntityState& eb = vb[j];
        if (ea.id == eb.id) {
            // Present in BOTH — interpolate.
            EntityState s{};
            s.id      = ea.id;
            s.pos[0]  = lerp_f32(ea.pos[0], eb.pos[0], t);
            s.pos[1]  = lerp_f32(ea.pos[1], eb.pos[1], t);
            s.pos[2]  = lerp_f32(ea.pos[2], eb.pos[2], t);
            s.yaw_deg = lerp_yaw_deg(ea.yaw_deg, eb.yaw_deg, t);
            out.push_back(s);
            ++i;
            ++j;
        } else if (ea.id < eb.id) {
            // In A only — pass through at its known value (no extrapolation).
            out.push_back(ea);
            ++i;
        } else {
            // In B only — pass through at its known value (no extrapolation).
            out.push_back(eb);
            ++j;
        }
    }
    // Drain whichever side still has tail ids (each present on one side only).
    for (; i < va.size(); ++i) out.push_back(va[i]);
    for (; j < vb.size(); ++j) out.push_back(vb[j]);
}

// ──────────────────────────────────────────────────────────────────────────
// sample — bracket `render_time_s`, clamp at the endpoints, interpolate.
// ──────────────────────────────────────────────────────────────────────────
bool SnapshotInterpBuffer::sample(f64 render_time_s,
                                  std::vector<EntityState>& out) const {
    out.clear();
    if (snaps_.empty()) return false;

    const Snapshot& oldest = snaps_.front();
    const Snapshot& newest = snaps_.back();

    // ── Clamp policy: outside the buffered span, emit the nearest endpoint
    //    snapshot verbatim. NO extrapolation past either end. ──────────────────
    if (render_time_s <= oldest.time) {
        copy_into(oldest, out);
        return true;
    }
    if (render_time_s >= newest.time) {
        copy_into(newest, out);
        return true;
    }

    // ── Find the bracket A,B with A.time <= render_time <= B.time. The ring is
    //    time-sorted, so the first snapshot whose time exceeds render_time is B
    //    and its predecessor is A. (We are strictly inside the span here, so
    //    such a B exists and is not the very first element.) ───────────────────
    usize bi = 1;
    while (bi < snaps_.size() && snaps_[bi].time < render_time_s) ++bi;
    // snaps_[bi].time >= render_time_s. Guard the (degenerate) index just in
    // case; the endpoint clamps above make bi <= size()-1 here.
    if (bi >= snaps_.size()) bi = snaps_.size() - 1;

    const Snapshot& a = snaps_[bi - 1];
    const Snapshot& b = snaps_[bi];

    // t = (render_time - A.time) / (B.time - A.time); zero-length interval -> 0;
    // clamp to [0,1]. f32 is sufficient for the cosmetic LERP and keeps the math
    // in the strict-FP single-precision lane the rest of the codec uses.
    const f64 span = b.time - a.time;
    f32 t = 0.f;
    if (span > 0.0) {
        t = static_cast<f32>((render_time_s - a.time) / span);
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
    }

    interp_into(a, b, t, out);
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// clear — drop every buffered snapshot, keep the capacity + reserved storage.
// ──────────────────────────────────────────────────────────────────────────
void SnapshotInterpBuffer::clear() noexcept {
    snaps_.clear();
}

}  // namespace psynder::net
