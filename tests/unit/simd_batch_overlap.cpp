// SPDX-License-Identifier: MIT
// Psynder — Lane 03 unit tests for the batch broadphase-overlap kernel
// (engine/simd/BatchOverlap). Covers the invariants the broadphase leans on:
//
//   1. Hand-checked membership — a tiny set where we know exactly which
//      points sit inside the query sphere.
//   2. Bit-identity — the SIMD mask equals the scalar reference mask BYTE
//      FOR BYTE on a large deterministic batch, including a count that is
//      not a multiple of the SIMD width (tail correctness). No RNG: the
//      layout is a fixed i*0.37-style formula so the test is reproducible.
//   3. Indices form — sphere_overlap_indices returns the ascending inside
//      indices, matching the set bits of the mask.
//   4. Scale — 100k points complete and the count is identical across two
//      runs (determinism).
//   5. Empty batch — returns 0 and writes nothing.
//
// All compares are exact (u8 masks / integer counts), so no floating-point
// tolerance is involved and Catch::Approx is not needed here.

#include "simd/BatchOverlap.h"
#include "simd/Dispatch.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

using namespace psynder;

namespace {

// Deterministic SoA point layout: coordinate i is a fixed closed-form of i,
// no RNG. The 0.37 / 0.19 / 0.11 strides spread points across a volume that
// straddles the query sphere so both inside and outside lanes occur.
void fill_points(std::size_t n, std::vector<f32>& xs, std::vector<f32>& ys,
                 std::vector<f32>& zs) {
    xs.resize(n);
    ys.resize(n);
    zs.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const f32 fi = static_cast<f32>(i);
        xs[i] = std::fmod(fi * 0.37f, 20.0f) - 10.0f;
        ys[i] = std::fmod(fi * 0.19f, 20.0f) - 10.0f;
        zs[i] = std::fmod(fi * 0.11f, 20.0f) - 10.0f;
    }
}

}  // namespace

TEST_CASE("sphere overlap hand-checked membership", "[simd]") {
    simd::dispatch_init();

    // Query sphere at the origin, radius 2 (r*r = 4). Six points placed by
    // hand: indices 0,1,4 inside, 2,3,5 outside.
    const std::vector<f32> xs{0.0f, 1.0f, 3.0f,  0.0f, -1.0f, 2.5f};
    const std::vector<f32> ys{0.0f, 1.0f, 0.0f,  5.0f,  1.0f, 0.0f};
    const std::vector<f32> zs{0.0f, 0.0f, 0.0f,  0.0f,  0.0f, 0.0f};
    std::vector<u8>        inside(xs.size(), 0xAB);  // poison fill

    const usize cnt = simd::sphere_overlap_mask(
        0.0f, 0.0f, 0.0f, 2.0f, xs, ys, zs, inside);

    REQUIRE(inside[0] == 1u);  // d2 = 0
    REQUIRE(inside[1] == 1u);  // d2 = 2
    REQUIRE(inside[2] == 0u);  // d2 = 9
    REQUIRE(inside[3] == 0u);  // d2 = 25
    REQUIRE(inside[4] == 1u);  // d2 = 2
    REQUIRE(inside[5] == 0u);  // d2 = 6.25
    REQUIRE(cnt == 3u);
}

TEST_CASE("sphere overlap boundary point counts as inside", "[simd]") {
    simd::dispatch_init();

    // A point exactly on the surface: d2 == r2. The predicate is d2 <= r2,
    // so it must be counted inside.
    const std::vector<f32> xs{3.0f};
    const std::vector<f32> ys{0.0f};
    const std::vector<f32> zs{0.0f};
    std::vector<u8>        inside(1, 0u);

    const usize cnt = simd::sphere_overlap_mask(
        0.0f, 0.0f, 0.0f, 3.0f, xs, ys, zs, inside);
    REQUIRE(inside[0] == 1u);
    REQUIRE(cnt == 1u);
}

TEST_CASE("sphere overlap SIMD mask is bit identical to scalar with tail",
          "[simd]") {
    simd::dispatch_init();

    // 1003 is deliberately NOT a multiple of 8 or 4 — exercises the scalar
    // tail of the vector path.
    const std::size_t n = 1003;
    std::vector<f32>  xs, ys, zs;
    fill_points(n, xs, ys, zs);

    const f32 cx = 1.5f, cy = -2.0f, cz = 0.5f, r = 4.5f;

    std::vector<u8> simd_mask(n, 0u);
    std::vector<u8> ref_mask(n, 0u);

    const usize simd_cnt = simd::sphere_overlap_mask(
        cx, cy, cz, r, xs, ys, zs, simd_mask);
    const usize ref_cnt = simd::sphere_overlap_scalar(
        cx, cy, cz, r, xs, ys, zs, ref_mask);

    REQUIRE(simd_cnt == ref_cnt);
    // Byte-for-byte identity across the whole batch including the tail.
    bool identical = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (simd_mask[i] != ref_mask[i]) { identical = false; break; }
    }
    REQUIRE(identical);

    // Sanity: the deterministic layout produces a non-trivial mix (not all
    // inside, not all outside) so the bit-identity check is meaningful.
    REQUIRE(simd_cnt > 0u);
    REQUIRE(simd_cnt < n);
}

TEST_CASE("sphere overlap indices match the mask in ascending order",
          "[simd]") {
    simd::dispatch_init();

    const std::size_t n = 257;  // not a width multiple
    std::vector<f32>  xs, ys, zs;
    fill_points(n, xs, ys, zs);

    const f32 cx = 0.0f, cy = 0.0f, cz = 0.0f, r = 5.0f;

    std::vector<u8> mask(n, 0u);
    const usize     mask_cnt = simd::sphere_overlap_mask(
        cx, cy, cz, r, xs, ys, zs, mask);

    std::vector<u32> indices(n, 0u);  // capacity >= count
    const usize      idx_cnt = simd::sphere_overlap_indices(
        cx, cy, cz, r, xs, ys, zs, indices);

    REQUIRE(idx_cnt == mask_cnt);

    // The written indices must be exactly the set bits of the mask, in
    // strictly ascending order.
    bool ok    = true;
    u32  prev  = 0u;
    bool first = true;
    for (usize k = 0; k < idx_cnt; ++k) {
        const u32 ix = indices[k];
        if (ix >= n || mask[ix] != 1u) { ok = false; break; }
        if (!first && !(ix > prev)) { ok = false; break; }
        prev  = ix;
        first = false;
    }
    REQUIRE(ok);

    // And every set mask bit is present (count already matched, ascending
    // + in-bounds + unique implies a bijection).
    usize mask_set = 0;
    for (usize i = 0; i < n; ++i) mask_set += mask[i];
    REQUIRE(mask_set == idx_cnt);
}

TEST_CASE("sphere overlap indices respect output capacity", "[simd]") {
    simd::dispatch_init();

    // Many points inside, but the output buffer only holds a few — the
    // function must stop at capacity and report exactly that many.
    const std::size_t n = 64;
    std::vector<f32>  xs(n, 0.0f), ys(n, 0.0f), zs(n, 0.0f);  // all at origin

    std::vector<u32> indices(5, 0xFFFFFFFFu);
    const usize      cnt = simd::sphere_overlap_indices(
        0.0f, 0.0f, 0.0f, 1.0f, xs, ys, zs, indices);

    REQUIRE(cnt == 5u);
    for (usize k = 0; k < 5; ++k) REQUIRE(indices[k] == static_cast<u32>(k));
}

TEST_CASE("sphere overlap scales to 100k points deterministically",
          "[simd]") {
    simd::dispatch_init();

    const std::size_t n = 100000;
    std::vector<f32>  xs, ys, zs;
    fill_points(n, xs, ys, zs);

    const f32 cx = 2.0f, cy = 2.0f, cz = 2.0f, r = 6.0f;

    std::vector<u8> mask_a(n, 0u);
    std::vector<u8> mask_b(n, 0u);

    const usize cnt_a = simd::sphere_overlap_mask(
        cx, cy, cz, r, xs, ys, zs, mask_a);
    const usize cnt_b = simd::sphere_overlap_mask(
        cx, cy, cz, r, xs, ys, zs, mask_b);

    // Two runs over the same input give the identical count and mask.
    REQUIRE(cnt_a == cnt_b);
    bool same = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (mask_a[i] != mask_b[i]) { same = false; break; }
    }
    REQUIRE(same);

    // And the vector result still matches the scalar reference at scale.
    std::vector<u8> ref(n, 0u);
    const usize     cnt_ref = simd::sphere_overlap_scalar(
        cx, cy, cz, r, xs, ys, zs, ref);
    REQUIRE(cnt_a == cnt_ref);
    bool ref_same = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (mask_a[i] != ref[i]) { ref_same = false; break; }
    }
    REQUIRE(ref_same);
}

TEST_CASE("sphere overlap empty batch returns zero", "[simd]") {
    simd::dispatch_init();

    std::span<const f32> empty_xs;
    std::span<const f32> empty_ys;
    std::span<const f32> empty_zs;
    std::span<u8>        empty_out;

    const usize cnt = simd::sphere_overlap_mask(
        0.0f, 0.0f, 0.0f, 1.0f, empty_xs, empty_ys, empty_zs, empty_out);
    REQUIRE(cnt == 0u);

    std::span<u32> empty_idx;
    const usize    idx_cnt = simd::sphere_overlap_indices(
        0.0f, 0.0f, 0.0f, 1.0f, empty_xs, empty_ys, empty_zs, empty_idx);
    REQUIRE(idx_cnt == 0u);
}

TEST_CASE("sphere overlap uses the minimum of the span sizes", "[simd]") {
    simd::dispatch_init();

    // Ragged spans: only the first 3 points are fully described, and the
    // output buffer only holds 2 — so exactly 2 lanes are processed.
    const std::vector<f32> xs{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const std::vector<f32> ys{0.0f, 0.0f, 0.0f};
    const std::vector<f32> zs{0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<u8>        inside(2, 0xCDu);  // poison

    const usize cnt = simd::sphere_overlap_mask(
        0.0f, 0.0f, 0.0f, 1.0f, xs, ys, zs, inside);

    REQUIRE(cnt == 2u);
    REQUIRE(inside[0] == 1u);
    REQUIRE(inside[1] == 1u);
}
