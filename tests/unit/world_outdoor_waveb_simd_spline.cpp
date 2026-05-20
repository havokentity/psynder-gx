// SPDX-License-Identifier: MIT
// Psynder — Lane 13 (world-outdoor) Wave B unit tests.
//
// Two Wave-B deliverables (DESIGN.md §9.2, ADR-008):
//
//   1. SIMD fast-path for heightfield sampling / traversal. The load-bearing
//      invariant: the 8-wide kernels reproduce their scalar references per
//      lane (bilinear sampling, ray-vs-heightfield intersection, CDLOD chunk
//      LOD selection). The packet uses the same op order as the scalar code,
//      so lanes match to floating-point tolerance; LOD selection matches
//      exactly. The JobSystem batch drivers (march_rays / select_chunk_lods)
//      must equal a per-element scalar loop.
//
//   2. The spline track editor: insert / move / remove control points,
//      Catmull-Rom -> cubic Bezier emit that interpolates the knots,
//      arc-length parameterisation (constant-speed sampling), and banking
//      (per-knot + auto-from-curvature).
//
// All in one file because the lane is allowed exactly one new Wave-B test TU.

#include <catch2/catch_test_macros.hpp>

#include "world/outdoor/HeightfieldSimd_internal.h"
#include "world/outdoor/Heightmap_internal.h"
#include "world/outdoor/Raymarch_internal.h"
#include "world/outdoor/Spline_internal.h"
#include "world/outdoor/SplineEditor_internal.h"
#include "world/outdoor/Terrain.h"

#include "simd/Simd_internal.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pwo   = psynder::world::outdoor;
namespace pwod  = psynder::world::outdoor::detail;
namespace psimd = psynder::simd;

using psynder::f32;
using psynder::i32;
using psynder::u32;
using psynder::u8;

namespace {

// Smooth ramp heightmap: world-Y rises monotonically with (x + z), so ray
// crossings are sharp and unambiguous (no grazing that could flip a march
// step between scalar and SIMD).
std::vector<std::uint16_t> make_ramp(u32 w, u32 h, u32 per_cell) {
    std::vector<std::uint16_t> out(static_cast<std::size_t>(w) * h);
    for (u32 z = 0; z < h; ++z) {
        for (u32 x = 0; x < w; ++x) {
            const u32 v = (x + z) * per_cell;
            out[static_cast<std::size_t>(z) * w + x] =
                static_cast<std::uint16_t>(v > 0xFFFFu ? 0xFFFFu : v);
        }
    }
    return out;
}

// Varied (high-frequency) heightmap for the bilinear parity test.
std::vector<std::uint16_t> make_varied(u32 w, u32 h) {
    std::vector<std::uint16_t> out(static_cast<std::size_t>(w) * h);
    for (u32 z = 0; z < h; ++z) {
        for (u32 x = 0; x < w; ++x) {
            const u32 v = (x * 1664525u + z * 1013904223u + 12345u);
            out[static_cast<std::size_t>(z) * w + x] = static_cast<std::uint16_t>(v & 0xFFFFu);
        }
    }
    return out;
}

bool approx(f32 a, f32 b, f32 abs_tol, f32 rel_tol) noexcept {
    return std::fabs(a - b) <= abs_tol + rel_tol * std::fabs(b);
}

}  // namespace

// ───────────────────────── SIMD bilinear sampling ─────────────────────────

TEST_CASE("SIMD bilinear matches scalar sample_bilinear per lane",
          "[world_outdoor][waveb][simd]") {
    const u32 W = 64, H = 64;
    auto      raw = make_varied(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 2.0f;
    desc.height_scale = 0.05f;
    desc.heights      = raw.data();

    // Eight world-XZ probes: interior fractional positions plus one past the
    // map border (exercises the shared border-reads-0 path on both sides).
    std::array<f32, 8> wx{3.3f, 10.7f, 25.0f, 40.4f, 60.9f, 17.2f, 50.5f, 130.0f};
    std::array<f32, 8> wz{4.1f, 31.8f, 12.5f, 55.6f, 8.3f, 44.9f, 22.2f, 9.0f};

    const psimd::f32x8 hx8 =
        pwod::sample_bilinear_x8(desc, psimd::load_unaligned8(wx.data()),
                                 psimd::load_unaligned8(wz.data()));
    std::array<f32, 8> got{};
    psimd::store_unaligned8(got.data(), hx8);

    for (int i = 0; i < 8; ++i) {
        const f32 ref = pwod::sample_bilinear(desc, wx[static_cast<std::size_t>(i)],
                                              wz[static_cast<std::size_t>(i)]);
        INFO("lane " << i);
        REQUIRE(approx(got[static_cast<std::size_t>(i)], ref, 1.0e-2f, 1.0e-4f));
    }
}

// ─────────────────── ray-vs-heightfield packet intersection ───────────────

TEST_CASE("SIMD ray packet matches scalar march_ray per lane",
          "[world_outdoor][waveb][simd][raymarch]") {
    const u32 W = 96, H = 96;
    auto      raw = make_ramp(W, H, 15);  // world-Y = (x+z) * 15 * 0.02 = (x+z)*0.3 m
    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.02f;  // gentle plane: Y stays below the ~50-90 m origins
    desc.heights      = raw.data();

    const f32 step  = 0.5f;
    const f32 max_t = 400.0f;

    // 8 rays: 6 descending into the ramp from above at various XZ + angles,
    // 1 pointing up (clear miss), 1 starting underground (hit at t=0).
    std::array<pwod::HeightfieldRay, 8> rays{};
    rays[0] = {{10.0f, 60.0f, 10.0f}, psynder::math::normalize({1.0f, -0.6f, 0.2f})};
    rays[1] = {{20.0f, 80.0f, 15.0f}, psynder::math::normalize({0.5f, -0.8f, 0.4f})};
    rays[2] = {{35.0f, 90.0f, 30.0f}, psynder::math::normalize({0.3f, -1.0f, 0.1f})};
    rays[3] = {{50.0f, 70.0f, 5.0f}, psynder::math::normalize({-0.2f, -0.7f, 0.6f})};
    rays[4] = {{8.0f, 50.0f, 40.0f}, psynder::math::normalize({0.7f, -0.5f, -0.3f})};
    rays[5] = {{25.0f, 65.0f, 25.0f}, psynder::math::normalize({0.4f, -0.9f, 0.4f})};
    rays[6] = {{30.0f, 200.0f, 30.0f}, psynder::math::normalize({0.0f, 1.0f, 0.0f})};  // miss (up)
    rays[7] = {{40.0f, 1.0f, 40.0f}, psynder::math::normalize({0.1f, -0.1f, 0.1f})};   // underground

    std::array<pwod::RayMarchHit, 8> out{};
    pwod::march_packet8(desc, rays.data(), 8u, step, max_t, out.data());

    for (int i = 0; i < 8; ++i) {
        pwod::RayHit ref{};
        const bool   ref_hit = pwod::march_ray(desc, rays[static_cast<std::size_t>(i)].origin,
                                              rays[static_cast<std::size_t>(i)].dir, step, max_t,
                                              ref);
        INFO("ray " << i);
        REQUIRE(out[static_cast<std::size_t>(i)].hit == ref_hit);
        if (ref_hit) {
            REQUIRE(approx(out[static_cast<std::size_t>(i)].t, ref.t, 1.0e-2f, 1.0e-4f));
            REQUIRE(approx(out[static_cast<std::size_t>(i)].pos.x, ref.pos.x, 1.0e-2f, 1.0e-4f));
            REQUIRE(approx(out[static_cast<std::size_t>(i)].pos.y, ref.pos.y, 1.0e-2f, 1.0e-4f));
            REQUIRE(approx(out[static_cast<std::size_t>(i)].pos.z, ref.pos.z, 1.0e-2f, 1.0e-4f));
        }
    }
    // Sanity: ray 6 misses, ray 7 hits at t ~ 0.
    REQUIRE_FALSE(out[6].hit);
    REQUIRE(out[7].hit);
    REQUIRE(out[7].t < 1.0e-3f);
}

TEST_CASE("march_packet8 leaves a partial-count tail untouched",
          "[world_outdoor][waveb][simd][raymarch]") {
    const u32 W = 32, H = 32;
    auto      raw = make_ramp(W, H, 15);
    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.02f;
    desc.heights      = raw.data();

    std::array<pwod::RayMarchHit, 8> out{};
    for (auto& o : out) o.hit = true;  // poison; padded lanes must be reset/ignored
    std::array<pwod::HeightfieldRay, 3> rays{};
    rays[0] = {{4.0f, 40.0f, 4.0f}, psynder::math::normalize({0.5f, -0.8f, 0.2f})};
    rays[1] = {{8.0f, 40.0f, 8.0f}, psynder::math::normalize({0.3f, -0.9f, 0.3f})};
    rays[2] = {{12.0f, 200.0f, 12.0f}, psynder::math::normalize({0.0f, 1.0f, 0.0f})};

    pwod::march_packet8(desc, rays.data(), 3u, 0.5f, 200.0f, out.data());
    REQUIRE(out[0].hit);
    REQUIRE(out[1].hit);
    REQUIRE_FALSE(out[2].hit);  // up-ray, written by the kernel as no-hit
}

TEST_CASE("march_rays driver matches scalar march_ray for every ray",
          "[world_outdoor][waveb][simd][jobs]") {
    const u32 W = 80, H = 80;
    auto      raw = make_ramp(W, H, 15);
    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.02f;
    desc.heights      = raw.data();

    const f32 step  = 0.5f;
    const f32 max_t = 300.0f;

    // 600 rays > kRayParallelThreshold, so the JobSystem parallel_for branch
    // runs (the Phase-0 stub executes the body synchronously; the result must
    // still equal a per-ray scalar march).
    std::vector<pwod::HeightfieldRay> rays;
    rays.reserve(600);
    for (u32 i = 0; i < 600; ++i) {
        const f32 fx  = 6.0f + static_cast<f32>(i % 60);
        const f32 fz  = 6.0f + static_cast<f32>((i / 60) % 60);
        const f32 dirx = 0.2f + 0.01f * static_cast<f32>(i % 7);
        rays.push_back({{fx, 70.0f, fz}, psynder::math::normalize({dirx, -0.9f, 0.15f})});
    }

    std::vector<pwod::RayMarchHit> out(rays.size());
    pwod::march_rays(desc, rays, step, max_t, out);

    for (std::size_t i = 0; i < rays.size(); ++i) {
        pwod::RayHit ref{};
        const bool   ref_hit = pwod::march_ray(desc, rays[i].origin, rays[i].dir, step, max_t, ref);
        INFO("ray " << i);
        REQUIRE(out[i].hit == ref_hit);
        if (ref_hit) {
            REQUIRE(approx(out[i].t, ref.t, 1.0e-2f, 1.0e-4f));
        }
    }
}

// ───────────────────────── CDLOD chunk LOD selection ──────────────────────

TEST_CASE("lod_for_distance follows doubling distance bands",
          "[world_outdoor][waveb][cdlod]") {
    const f32 leaf = 100.0f;
    REQUIRE(pwod::lod_for_distance(0.0f, leaf) == 0u);
    REQUIRE(pwod::lod_for_distance(99.0f, leaf) == 0u);
    REQUIRE(pwod::lod_for_distance(150.0f, leaf) == 1u);   // (100, 200]
    REQUIRE(pwod::lod_for_distance(250.0f, leaf) == 2u);   // (200, 400]
    REQUIRE(pwod::lod_for_distance(500.0f, leaf) == 3u);   // (400, 800]
    // Far beyond the last band clamps to kMaxLodLevels - 1.
    REQUIRE(pwod::lod_for_distance(1.0e9f, leaf) == pwod::kMaxLodLevels - 1u);
}

TEST_CASE("select_lods_x8 matches scalar lod_for_distance per lane",
          "[world_outdoor][waveb][cdlod][simd]") {
    const f32 leaf = 64.0f;
    // Distances chosen off the band boundaries so 1-ULP noise can't flip a LOD.
    std::array<f32, 8> dist{10.0f, 70.0f, 90.0f, 200.0f, 300.0f, 600.0f, 1500.0f, 9000.0f};
    std::array<u32, 8> got{};
    pwod::select_lods_x8(psimd::load_unaligned8(dist.data()), leaf, got.data());
    for (int i = 0; i < 8; ++i) {
        INFO("lane " << i);
        REQUIRE(got[static_cast<std::size_t>(i)] ==
                pwod::lod_for_distance(dist[static_cast<std::size_t>(i)], leaf));
    }
}

TEST_CASE("select_chunk_lods driver matches scalar per chunk",
          "[world_outdoor][waveb][cdlod][jobs]") {
    // 300 chunk AABBs > kChunkLodParallelThreshold, walking away from the eye.
    const psynder::math::Vec3 eye{0.0f, 0.0f, 0.0f};
    const f32                 leaf = 50.0f;
    std::vector<psynder::math::Aabb> bounds;
    bounds.reserve(300);
    for (u32 i = 0; i < 300; ++i) {
        const f32 cx = 3.0f + 2.7f * static_cast<f32>(i);  // off-boundary spacing
        psynder::math::Aabb b{};
        b.min = {cx - 1.0f, -1.0f, 5.0f};
        b.max = {cx + 1.0f, 1.0f, 7.0f};
        bounds.push_back(b);
    }

    std::vector<u8> lods(bounds.size());
    pwod::select_chunk_lods(bounds, eye, leaf, lods);

    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const f32 d   = pwod::chunk_center_distance(bounds[i], eye);
        const u32 ref = pwod::lod_for_distance(d, leaf);
        INFO("chunk " << i);
        REQUIRE(static_cast<u32>(lods[i]) == ref);
    }
}

// ───────────────────────────── Spline editor ──────────────────────────────

namespace {

pwod::SplineTrack make_square_track() {
    pwod::SplineTrack t{};
    pwod::append_point(t, {{0.0f, 0.0f, 0.0f}, 4.0f, 0.0f});
    pwod::append_point(t, {{40.0f, 0.0f, 0.0f}, 4.0f, 0.0f});
    pwod::append_point(t, {{40.0f, 0.0f, 40.0f}, 4.0f, 0.0f});
    pwod::append_point(t, {{0.0f, 0.0f, 40.0f}, 4.0f, 0.0f});
    return t;
}

bool vec_close(psynder::math::Vec3 a, psynder::math::Vec3 b, f32 tol) noexcept {
    return std::fabs(a.x - b.x) < tol && std::fabs(a.y - b.y) < tol && std::fabs(a.z - b.z) < tol;
}

}  // namespace

TEST_CASE("spline editor insert / move / remove edit the knot list",
          "[world_outdoor][waveb][spline][editor]") {
    pwod::SplineTrack t{};
    REQUIRE(pwod::append_point(t, {{0.0f, 0.0f, 0.0f}}) == 0u);
    REQUIRE(pwod::append_point(t, {{10.0f, 0.0f, 0.0f}}) == 1u);
    REQUIRE(t.points.size() == 2u);

    // Insert in the middle.
    REQUIRE(pwod::insert_point(t, 1u, {{5.0f, 0.0f, 0.0f}}));
    REQUIRE(t.points.size() == 3u);
    REQUIRE(t.points[1].position.x == 5.0f);

    // Insert at end (index == size) is an append.
    REQUIRE(pwod::insert_point(t, t.points.size(), {{20.0f, 0.0f, 0.0f}}));
    REQUIRE(t.points.size() == 4u);

    // Move.
    REQUIRE(pwod::move_point(t, 0u, psynder::math::Vec3{-1.0f, 2.0f, 3.0f}));
    REQUIRE(vec_close(t.points[0].position, psynder::math::Vec3{-1.0f, 2.0f, 3.0f}, 1.0e-6f));

    // Remove.
    REQUIRE(pwod::remove_point(t, 1u));
    REQUIRE(t.points.size() == 3u);

    // Out-of-range ops are no-ops that report failure.
    REQUIRE_FALSE(pwod::insert_point(t, 99u, {}));
    REQUIRE_FALSE(pwod::move_point(t, 99u, {}));
    REQUIRE_FALSE(pwod::remove_point(t, 99u));
    REQUIRE(t.points.size() == 3u);
}

TEST_CASE("to_segments interpolates the knots (C0 continuity)",
          "[world_outdoor][waveb][spline]") {
    auto       t    = make_square_track();
    const auto segs = pwod::to_segments(t);  // open: 4 knots -> 3 segments
    REQUIRE(segs.size() == 3u);

    for (std::size_t i = 0; i < segs.size(); ++i) {
        // Catmull-Rom -> Bezier: each segment starts/ends exactly on its knots.
        REQUIRE(vec_close(pwod::bezier_eval(segs[i], 0.0f), t.points[i].position, 1.0e-4f));
        REQUIRE(vec_close(pwod::bezier_eval(segs[i], 1.0f), t.points[i + 1].position, 1.0e-4f));
    }
}

TEST_CASE("collinear knots produce a straight road",
          "[world_outdoor][waveb][spline]") {
    pwod::SplineTrack t{};
    for (int i = 0; i < 5; ++i) {
        pwod::append_point(t, {{static_cast<f32>(i) * 10.0f, 0.0f, 0.0f}});
    }
    const auto segs = pwod::to_segments(t);
    REQUIRE(segs.size() == 4u);
    // Sample along the curve: a straight X-axis track stays on z = y = 0.
    for (const auto& s : segs) {
        for (int k = 0; k <= 8; ++k) {
            const auto p = pwod::bezier_eval(s, static_cast<f32>(k) / 8.0f);
            REQUIRE(std::fabs(p.y) < 1.0e-4f);
            REQUIRE(std::fabs(p.z) < 1.0e-4f);
        }
    }
}

TEST_CASE("closed track wraps into a loop of segments",
          "[world_outdoor][waveb][spline]") {
    auto t   = make_square_track();
    t.closed = true;
    const auto segs = pwod::to_segments(t);  // closed: 4 knots -> 4 segments
    REQUIRE(segs.size() == 4u);
    // The last segment closes back onto the first knot.
    REQUIRE(vec_close(pwod::bezier_eval(segs.back(), 1.0f), t.points[0].position, 1.0e-4f));
}

TEST_CASE("arc-length sampling is constant speed and spans the track",
          "[world_outdoor][waveb][spline][arclength]") {
    auto       t   = make_square_track();
    const auto tab = pwod::build_arc_table(t, 48);
    const f32  total = pwod::total_length(tab);
    REQUIRE(total > 0.0f);

    // Endpoints land on the first / last knot (open track).
    const auto f0 = pwod::sample_at_arc_length(tab, 0.0f);
    const auto f1 = pwod::sample_at_arc_length(tab, total);
    REQUIRE(vec_close(f0.pos, t.points.front().position, 1.0e-2f));
    REQUIRE(vec_close(f1.pos, t.points.back().position, 1.0e-2f));

    // Constant speed: equal arc-length steps advance ~equal world distance.
    const int n = 40;
    f32       prev_gap = -1.0f;
    psynder::math::Vec3 prev = pwod::sample_at_arc_length(tab, 0.0f).pos;
    for (int i = 1; i <= n; ++i) {
        const f32  s   = total * static_cast<f32>(i) / static_cast<f32>(n);
        const auto cur = pwod::sample_at_arc_length(tab, s).pos;
        const f32  gap = psynder::math::length(psynder::math::sub(cur, prev));
        const f32  expected = total / static_cast<f32>(n);
        // Near the ideal even spacing (slack for LUT discretisation + curvature).
        REQUIRE(gap > expected * 0.65f);
        REQUIRE(gap < expected * 1.35f);
        prev_gap = gap;
        prev     = cur;
    }
    REQUIRE(prev_gap > 0.0f);
}

TEST_CASE("auto_bank is zero on a straight road and banks into corners",
          "[world_outdoor][waveb][spline][banking]") {
    // Straight track: no curvature -> no banking anywhere.
    pwod::SplineTrack straight{};
    for (int i = 0; i < 5; ++i) {
        pwod::append_point(straight, {{static_cast<f32>(i) * 10.0f, 0.0f, 0.0f}});
    }
    pwod::auto_bank(straight, psynder::math::kHalfPi * 0.5f, 1.0f);
    for (const auto& p : straight.points) REQUIRE(std::fabs(p.banking_rad) < 1.0e-5f);

    // A right-angle left turn: the corner knot gets nonzero banking; the
    // straight endpoints stay flat.
    pwod::SplineTrack corner{};
    pwod::append_point(corner, {{0.0f, 0.0f, 0.0f}});
    pwod::append_point(corner, {{10.0f, 0.0f, 0.0f}});   // travel +X
    pwod::append_point(corner, {{10.0f, 0.0f, 10.0f}});  // then turn to +Z
    pwod::auto_bank(corner, psynder::math::kHalfPi, 1.0f);
    REQUIRE(std::fabs(corner.points[0].banking_rad) < 1.0e-5f);
    REQUIRE(std::fabs(corner.points[2].banking_rad) < 1.0e-5f);
    REQUIRE(std::fabs(corner.points[1].banking_rad) > 1.0e-3f);
}

TEST_CASE("banking rolls the sampled road frame off vertical",
          "[world_outdoor][waveb][spline][banking]") {
    // Straight track but with explicit banking authored at the knots.
    pwod::SplineTrack t{};
    pwod::append_point(t, {{0.0f, 0.0f, 0.0f}, 5.0f, 0.0f});
    pwod::append_point(t, {{30.0f, 0.0f, 0.0f}, 5.0f, psynder::math::kHalfPi * 0.5f});
    pwod::append_point(t, {{60.0f, 0.0f, 0.0f}, 7.0f, 0.0f});

    const auto tab   = pwod::build_arc_table(t, 32);
    const f32  total = pwod::total_length(tab);

    // Unbanked at the very start: up ~ +Y.
    const auto start = pwod::sample_at_arc_length(tab, 0.0f);
    REQUIRE(start.up.y > 0.99f);
    REQUIRE(std::fabs(start.banking_rad) < 1.0e-3f);

    // Mid-track the banking has rolled in: up tilts away from +Y.
    const auto mid = pwod::sample_at_arc_length(tab, total * 0.5f);
    REQUIRE(std::fabs(mid.banking_rad) > 1.0e-2f);
    REQUIRE(mid.up.y < 0.999f);
    REQUIRE(std::fabs(mid.right.y) > 1.0e-2f);  // banking lifts the right edge off the ground plane

    // Half-width interpolates between authored knot widths (5 .. 7).
    REQUIRE(mid.half_width > 4.9f);
    REQUIRE(mid.half_width < 7.1f);
}
