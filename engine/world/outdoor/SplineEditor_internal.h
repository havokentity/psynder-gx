// SPDX-License-Identifier: MIT
// Psynder — spline track editor (DESIGN.md §9.2, ADR-008 racing tracks).
//
// A racing track is authored as an ordered list of control points (knots) the
// road passes through. The editor only ever mutates that knot list (insert /
// move / remove); the cubic-Bezier road segments are *derived* on demand via a
// uniform Catmull-Rom -> Bezier conversion, so the curve always interpolates
// the knots and stays C1 across joins. This is the layer the editor (lane 22)
// drives; its emitted segments are the already-public `SplineRoadSegment` the
// renderer (lane 09) and physics (lane 13) consume, and the per-segment strip
// extrusion stays in Spline_internal.h.
//
// Frozen-header note: the editor state types (`SplineTrack`,
// `TrackControlPoint`) live in `detail` because Terrain.h is frozen (Wave A).
// The editor's *output* is the public `SplineRoadSegment` list, so the public
// contract is unchanged. If lane 22 needs these types in the public surface,
// that is a coordinated Terrain.h change (Issue against the orchestrator).
//
// Beyond editing this provides:
//   - arc-length parameterisation: constant-speed sampling along the whole
//     track (even road-texture spacing, AI racing line, physics waypoints);
//   - banking: per-knot banking flows into the derived frame, interpolated
//     smoothly along each segment, plus an auto-bank-from-curvature helper.
//
// Header-only so the unit test exercises it without linking the world_outdoor
// static lib (matching the rest of the lane's internal headers).

#pragma once

#include "world/outdoor/Spline_internal.h"
#include "world/outdoor/Terrain.h"

#include "core/Types.h"
#include "math/Math.h"

#include <cstddef>
#include <vector>

namespace psynder::world::outdoor::detail {

// A knot the track interpolates. half_width + banking are authored per-knot and
// interpolated along each derived segment. 1 unit = 1 metre; banking in rad.
struct TrackControlPoint {
    math::Vec3 position{0, 0, 0};
    f32        half_width  = 4.0f;
    f32        banking_rad = 0.0f;
};

// An editable racing-track spline. `closed` wraps the last knot back to the
// first (a loop circuit). Bezier segments are derived from `points`.
struct SplineTrack {
    std::vector<TrackControlPoint> points;
    bool                           closed = false;
};

// ─── Editing ops ──────────────────────────────────────────────────────────
// All return false (and leave the track untouched) on an out-of-range index.
// `insert_point` accepts index == size (an append).
inline usize append_point(SplineTrack& t, const TrackControlPoint& p) {
    t.points.push_back(p);
    return t.points.size() - 1;
}

inline bool insert_point(SplineTrack& t, usize index, const TrackControlPoint& p) {
    if (index > t.points.size()) return false;
    t.points.insert(t.points.begin() + static_cast<isize>(index), p);
    return true;
}

inline bool move_point(SplineTrack& t, usize index, math::Vec3 new_pos) {
    if (index >= t.points.size()) return false;
    t.points[index].position = new_pos;
    return true;
}

inline bool remove_point(SplineTrack& t, usize index) {
    if (index >= t.points.size()) return false;
    t.points.erase(t.points.begin() + static_cast<isize>(index));
    return true;
}

// Knot index with end behaviour: closed tracks wrap modulo N; open tracks
// clamp to the first / last knot (so the end tangents are well-defined). The
// `i` argument is signed because Catmull-Rom reaches one knot either side.
inline usize knot_index(const SplineTrack& t, isize i) noexcept {
    const isize n = static_cast<isize>(t.points.size());
    if (t.closed) {
        return static_cast<usize>(((i % n) + n) % n);
    }
    if (i < 0) return 0;
    if (i >= n) return static_cast<usize>(n - 1);
    return static_cast<usize>(i);
}

// ─── Catmull-Rom -> cubic Bezier ────────────────────────────────────────
// Emits one SplineRoadSegment per knot interval (N-1 open, N closed). Needs
// >= 2 knots. The control points use the uniform Catmull-Rom tangent
// m_i = (P[i+1] - P[i-1]) / 2, mapped to Bezier handles at P +/- m/3 (the
// /6 below folds the /2 and /3 together), so each segment interpolates its two
// endpoint knots and tangents are continuous across joins. half_width and
// banking are the average of the two endpoint knots (SplineRoadSegment carries
// one value each; smooth per-t interpolation is available via the arc table's
// sample_at_arc_length).
inline std::vector<SplineRoadSegment> to_segments(const SplineTrack& t) {
    std::vector<SplineRoadSegment> out;
    const usize n = t.points.size();
    if (n < 2) return out;

    const usize seg_count = t.closed ? n : (n - 1);
    out.reserve(seg_count);
    for (usize s = 0; s < seg_count; ++s) {
        const isize si = static_cast<isize>(s);
        const math::Vec3 pa = t.points[knot_index(t, si - 1)].position;
        const math::Vec3 pb = t.points[knot_index(t, si)].position;
        const math::Vec3 pc = t.points[knot_index(t, si + 1)].position;
        const math::Vec3 pd = t.points[knot_index(t, si + 2)].position;

        SplineRoadSegment seg{};
        seg.p0 = pb;
        seg.p1 = math::add(pb, math::mul(math::sub(pc, pa), 1.0f / 6.0f));
        seg.p2 = math::sub(pc, math::mul(math::sub(pd, pb), 1.0f / 6.0f));
        seg.p3 = pc;

        const TrackControlPoint& kb = t.points[knot_index(t, si)];
        const TrackControlPoint& kc = t.points[knot_index(t, si + 1)];
        seg.half_width  = (kb.half_width + kc.half_width) * 0.5f;
        seg.banking_rad = (kb.banking_rad + kc.banking_rad) * 0.5f;
        out.push_back(seg);
    }
    return out;
}

// ─── Arc-length parameterisation ──────────────────────────────────────────
struct ArcSample {
    f32 s   = 0.0f;  // cumulative arc length (metres) at this sample
    u32 seg = 0;     // segment index
    f32 t   = 0.0f;  // segment-local parameter in [0,1]
};

// Per-segment endpoint half-width / banking, so sampling can interpolate them
// smoothly along the segment (the SplineRoadSegment itself only stores the
// per-segment average).
struct SegmentEnds {
    f32 half_width0 = 4.0f, half_width1 = 4.0f;
    f32 banking0    = 0.0f, banking1    = 0.0f;
};

struct ArcTable {
    std::vector<SplineRoadSegment> segments;
    std::vector<SegmentEnds>       ends;     // parallel to `segments`
    std::vector<ArcSample>         samples;  // monotonic non-decreasing in `s`
};

// Build the cumulative arc-length table by sampling each segment at
// `samples_per_seg` sub-steps and summing chord lengths. The join sample
// between consecutive segments is emitted once (it is the same world point on
// both, contributing zero length).
inline ArcTable build_arc_table(const SplineTrack& t, u32 samples_per_seg = 32) {
    ArcTable tab;
    tab.segments = to_segments(t);
    if (tab.segments.empty()) return tab;

    const usize seg_count = tab.segments.size();
    tab.ends.reserve(seg_count);
    for (usize s = 0; s < seg_count; ++s) {
        const isize               si = static_cast<isize>(s);
        const TrackControlPoint&  kb = t.points[knot_index(t, si)];
        const TrackControlPoint&  kc = t.points[knot_index(t, si + 1)];
        tab.ends.push_back(SegmentEnds{kb.half_width, kc.half_width, kb.banking_rad, kc.banking_rad});
    }

    if (samples_per_seg < 1) samples_per_seg = 1;
    f32        cum  = 0.0f;
    math::Vec3 prev{0, 0, 0};
    bool       have = false;
    for (usize si = 0; si < seg_count; ++si) {
        const u32 first = (si == 0) ? 0u : 1u;  // skip duplicate join sample
        for (u32 k = first; k <= samples_per_seg; ++k) {
            const f32        tt = static_cast<f32>(k) / static_cast<f32>(samples_per_seg);
            const math::Vec3 p  = bezier_eval(tab.segments[si], tt);
            if (have) cum += math::length(math::sub(p, prev));
            tab.samples.push_back(ArcSample{cum, static_cast<u32>(si), tt});
            prev = p;
            have = true;
        }
    }
    return tab;
}

inline f32 total_length(const ArcTable& tab) noexcept {
    return tab.samples.empty() ? 0.0f : tab.samples.back().s;
}

// A road frame at one point along the track: world position, unit tangent, and
// the (right, up) basis after banking, plus the interpolated half-width.
struct TrackFrame {
    math::Vec3 pos{0, 0, 0};
    math::Vec3 tangent{0, 0, 0};
    math::Vec3 right{0, 0, 0};
    math::Vec3 up{0, 0, 0};
    f32        half_width  = 0.0f;
    f32        banking_rad = 0.0f;
};

// Sample the track at arc length `s` (metres). `s` is clamped to
// [0, total_length]. Constant-speed in `s`: equal `s` increments advance equal
// world distance (to within the arc-table resolution). half-width and banking
// are interpolated smoothly across the bracketing samples' segment.
inline TrackFrame sample_at_arc_length(const ArcTable& tab, f32 s) {
    TrackFrame f{};
    if (tab.samples.size() < 2 || tab.segments.empty()) return f;

    const f32 total = tab.samples.back().s;
    if (s < 0.0f) s = 0.0f;
    if (s > total) s = total;

    // Largest j with samples[j].s <= s, bracketed by j+1.
    usize lo = 0;
    usize hi = tab.samples.size() - 1;
    while (lo < hi) {
        const usize mid = (lo + hi + 1) / 2;
        if (tab.samples[mid].s <= s) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    usize j = lo;
    if (j + 1 >= tab.samples.size()) j = tab.samples.size() - 2;

    const ArcSample& a    = tab.samples[j];
    const ArcSample& b    = tab.samples[j + 1];
    const f32        ds   = b.s - a.s;
    const f32        frac = ds > 0.0f ? (s - a.s) / ds : 0.0f;

    // A bracket usually lies inside one segment; at a join take the next
    // segment's start (t ~ 0) so we never read a stale segment index.
    u32 seg;
    f32 tt;
    if (a.seg == b.seg) {
        seg = a.seg;
        tt  = a.t + (b.t - a.t) * frac;
    } else {
        seg = b.seg;
        tt  = b.t;
    }

    const SplineRoadSegment& sg = tab.segments[seg];
    const SegmentEnds&       en = tab.ends[seg];
    const f32 banking = en.banking0 + (en.banking1 - en.banking0) * tt;

    // Reuse Spline_internal's frame_at by feeding it the interpolated banking.
    SplineRoadSegment banked = sg;
    banked.banking_rad       = banking;
    math::Vec3 right{0, 0, 0};
    math::Vec3 up{0, 0, 0};
    frame_at(banked, tt, right, up);

    f.pos         = bezier_eval(sg, tt);
    f.tangent     = math::normalize(bezier_tangent(sg, tt));
    f.right       = right;
    f.up          = up;
    f.half_width  = en.half_width0 + (en.half_width1 - en.half_width0) * tt;
    f.banking_rad = banking;
    return f;
}

// ─── Banking from curvature (NFS-style bank-into-corners) ──────────────────
// Sets each knot's banking from the local turn: the signed XZ turn between the
// incoming and outgoing chord directions (cross-product Y, +Y = left turn for
// the right-handed up = +Y convention), scaled by `sensitivity` and clamped to
// +/- max_bank_rad. Straight (collinear) knots get zero banking. Open-track
// endpoints have no curvature, so they are set to zero.
inline void auto_bank(SplineTrack& t, f32 max_bank_rad, f32 sensitivity = 1.0f) {
    const usize n = t.points.size();
    if (n < 3) {
        for (auto& p : t.points) p.banking_rad = 0.0f;
        return;
    }

    const auto clamp_bank = [max_bank_rad](f32 v) noexcept {
        if (v < -max_bank_rad) return -max_bank_rad;
        if (v > max_bank_rad) return max_bank_rad;
        return v;
    };
    const auto bank_at = [&](usize prev, usize cur, usize next) noexcept {
        const math::Vec3 in  = math::normalize(math::sub(t.points[cur].position, t.points[prev].position));
        const math::Vec3 out = math::normalize(math::sub(t.points[next].position, t.points[cur].position));
        const f32        turn = math::cross(in, out).y;  // signed turn in the XZ ground plane
        return clamp_bank(turn * sensitivity);
    };

    if (t.closed) {
        for (usize i = 0; i < n; ++i) {
            t.points[i].banking_rad = bank_at((i + n - 1) % n, i, (i + 1) % n);
        }
    } else {
        t.points[0].banking_rad     = 0.0f;
        t.points[n - 1].banking_rad = 0.0f;
        for (usize i = 1; i + 1 < n; ++i) {
            t.points[i].banking_rad = bank_at(i - 1, i, i + 1);
        }
    }
}

}  // namespace psynder::world::outdoor::detail
