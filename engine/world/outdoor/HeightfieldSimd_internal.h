// SPDX-License-Identifier: MIT
// Psynder — SIMD fast-path for heightfield sampling, ray-vs-heightfield
// intersection, and CDLOD chunk LOD selection (DESIGN.md §9.2, ADR-008).
//
// ADR-008 note: this is the *sampler / intersection* fast path for the
// existing CDLOD heightfield — NOT a second terrain backend. Per ADR-008 GX
// does not reintroduce a standalone raymarcher backend; the vectorised
// `march_packet8` below is the kernel the §8.2 hybrid shadow path and
// heightfield physics raycasts use against the same 16-bit heightmap the
// CDLOD mesh tessellates. `select_lods_*` is the per-chunk LOD selection the
// CDLOD pass runs before emitting draw items.
//
// The vectorisation axis is "N rays / N chunks at once". Because the heightmap
// is 16-bit (u16), address generation (texel index + corner fetch) stays
// scalar — that is memory-bound regardless of width — while the filter math,
// ray stepping, hit test, bisection refine, and LOD band selection are packed
// 8-wide. Each kernel reproduces its scalar reference per lane to within
// floating-point tolerance: the packet uses explicit mul+add (separate
// intrinsic calls don't contract into an FMA), but the scalar reference may
// be FMA-contracted by the compiler — psynder_simd enables FMA on AVX2 hosts
// and this lane doesn't force -ffp-contract=off — so a lane can differ by
// ~1 ULP. CDLOD LOD selection is integer and matches exactly. The unit test
// pins this with a tight tolerance (and exact equality for LOD).
//
// Header-only so the unit test can exercise the kernels without linking the
// world_outdoor static lib (tests/unit/CMakeLists.txt links a fixed lane set,
// owned by the build-system maintainer). The SIMD kernels come from lane 03's
// inline back-end (engine/simd/Simd_internal.h) — a compile-time-resolved,
// header-only dependency; no new link edge (psynder_simd is already a PUBLIC
// dependency of this lane). Per-element batch parallelism goes through
// psynder::jobs::JobSystem (lane 04), matching the WorldState / audio mixer
// precedent.

#pragma once

#include "world/outdoor/Heightmap_internal.h"

#include "core/Types.h"
#include "jobs/JobSystem.h"
#include "math/Math.h"
#include "simd/Simd_internal.h"

#include <cmath>
#include <span>

namespace psynder::world::outdoor::detail {

// Below these element counts the batch drivers run serially on the caller;
// above, they dispatch to JobSystem::parallel_for. Grains are multiples of the
// 8-wide packet so a chunk never splits a packet across job boundaries.
inline constexpr usize kRayParallelThreshold     = 256;
inline constexpr usize kRayGrain                 = 256;
inline constexpr usize kChunkLodParallelThreshold = 256;
inline constexpr usize kChunkLodGrain            = 256;

// ─── 8-wide bilinear height sample ───────────────────────────────────────
// Eight world-XZ positions in metres → eight bilinear heights in metres.
// Matches scalar `sample_bilinear` per lane to fp tolerance: identical
// division / floor / frac (scalar, per lane) and a mul+add lerp (packed; the
// scalar reference may be FMA-contracted — see the header note). Border texels
// read 0 (the `sample_raw` convention), so out-of-map lanes return a flat
// horizon just like the scalar path.
PSY_FORCEINLINE simd::f32x8 sample_bilinear_x8(const HeightmapDesc& h, simd::f32x8 wx8,
                                               simd::f32x8 wz8) noexcept {
    if (!h.heights || h.size_x == 0 || h.size_z == 0 || !(h.spacing > 0.0f)) {
        return simd::broadcast8(0.0f);
    }

    f32 wx[8], wz[8];
    simd::store_unaligned8(wx, wx8);
    simd::store_unaligned8(wz, wz8);

    f32 h00[8], h10[8], h01[8], h11[8], tx[8], tz[8];
    for (int i = 0; i < 8; ++i) {
        const f32 fx = wx[i] / h.spacing;
        const f32 fz = wz[i] / h.spacing;
        const i32 x0 = static_cast<i32>(std::floor(fx));
        const i32 z0 = static_cast<i32>(std::floor(fz));
        tx[i]  = fx - static_cast<f32>(x0);
        tz[i]  = fz - static_cast<f32>(z0);
        h00[i] = height_at_texel(h, x0, z0);
        h10[i] = height_at_texel(h, x0 + 1, z0);
        h01[i] = height_at_texel(h, x0, z0 + 1);
        h11[i] = height_at_texel(h, x0 + 1, z0 + 1);
    }

    const simd::f32x8 vtx = simd::load_unaligned8(tx);
    const simd::f32x8 vtz = simd::load_unaligned8(tz);
    const simd::f32x8 v00 = simd::load_unaligned8(h00);
    const simd::f32x8 v10 = simd::load_unaligned8(h10);
    const simd::f32x8 v01 = simd::load_unaligned8(h01);
    const simd::f32x8 v11 = simd::load_unaligned8(h11);

    // hx0 = h00 + (h10 - h00) * tx ; explicit mul+add (not fused) mirrors the
    // scalar op order — parity is tolerance-based, see the header note.
    const simd::f32x8 hx0 = simd::add8(v00, simd::mul8(simd::sub8(v10, v00), vtx));
    const simd::f32x8 hx1 = simd::add8(v01, simd::mul8(simd::sub8(v11, v01), vtx));
    return simd::add8(hx0, simd::mul8(simd::sub8(hx1, hx0), vtz));
}

// ─── Ray-vs-heightfield packet intersection ──────────────────────────────
struct HeightfieldRay {
    math::Vec3 origin{0, 0, 0};
    math::Vec3 dir{0, 0, 0};
};

struct RayMarchHit {
    bool       hit    = false;
    f32        t      = 0.0f;       // along-ray parameter at the hit
    f32        height = 0.0f;       // terrain height at the hit
    math::Vec3 pos{0, 0, 0};        // world-space hit position
};

// March up to 8 rays (`count` in [0,8]) in lockstep through the heightfield.
// Fixed step + a single linear bisection refine — the same scheme as scalar
// `march_ray`, so each lane's (hit, t, pos, height) matches the scalar call on
// that ray. Lanes >= count are inert. All rays share one step schedule, so the
// along-ray parameter `t` is a uniform scalar; only the per-ray world position
// differs across lanes.
inline void march_packet8(const HeightmapDesc& h, const HeightfieldRay* rays, u32 count,
                          f32 step_metres, f32 max_t, RayMarchHit* out) noexcept {
    for (u32 i = 0; i < count && i < 8u; ++i) out[i] = RayMarchHit{};
    if (!h.heights || h.size_x == 0 || h.size_z == 0 || count == 0) return;
    if (!(step_metres > 0.0f)) step_metres = h.spacing > 0.0f ? h.spacing : 1.0f;

    f32 ox[8]{}, oy[8]{}, oz[8]{}, dx[8]{}, dy[8]{}, dz[8]{};
    for (u32 i = 0; i < count && i < 8u; ++i) {
        ox[i] = rays[i].origin.x;
        oy[i] = rays[i].origin.y;
        oz[i] = rays[i].origin.z;
        dx[i] = rays[i].dir.x;
        dy[i] = rays[i].dir.y;
        dz[i] = rays[i].dir.z;
    }
    const simd::f32x8 ox8 = simd::load_unaligned8(ox);
    const simd::f32x8 oy8 = simd::load_unaligned8(oy);
    const simd::f32x8 oz8 = simd::load_unaligned8(oz);
    const simd::f32x8 dx8 = simd::load_unaligned8(dx);
    const simd::f32x8 dy8 = simd::load_unaligned8(dy);
    const simd::f32x8 dz8 = simd::load_unaligned8(dz);

    // Lanes [0,count) active. Lane indices 0..7 are exact in f32, so a float
    // compare yields the right mask without an integer-compare intrinsic.
    const f32 lane_idx[8] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
    simd::mask8 active =
        simd::cmp_lt8(simd::load_unaligned8(lane_idx), simd::broadcast8(static_cast<f32>(count)));

    const simd::f32x8 zero = simd::broadcast8(0.0f);

    // t = 0: a lane already at/under the terrain is a hit at t = 0.
    const simd::f32x8 th0    = sample_bilinear_x8(h, ox8, oz8);
    const simd::mask8 under0 = simd::mask_and(simd::cmp_le8(oy8, th0), active);

    simd::f32x8 res_t    = zero;        // t at the hit (0 for under-origin lanes)
    simd::mask8 hit_mask = under0;
    active               = simd::mask_and(active, simd::mask_not(under0));

    simd::f32x8 prev_t  = zero;
    simd::f32x8 prev_ry = oy8;
    simd::f32x8 prev_th = th0;

    f32 t = step_metres;
    while (t <= max_t) {
        const simd::f32x8 t8  = simd::broadcast8(t);
        const simd::f32x8 wx8 = simd::add8(ox8, simd::mul8(dx8, t8));
        const simd::f32x8 wy8 = simd::add8(oy8, simd::mul8(dy8, t8));
        const simd::f32x8 wz8 = simd::add8(oz8, simd::mul8(dz8, t8));
        const simd::f32x8 th8 = sample_bilinear_x8(h, wx8, wz8);

        const simd::mask8 hit_now = simd::mask_and(simd::cmp_le8(wy8, th8), active);
        if (simd::any_of(hit_now)) {
            // frac = clamp(dy_a / (dy_a - dy_b), 0, 1); denom <= 0 → 0. Same
            // order as the scalar reference, so the refined t matches it to
            // fp tolerance (modulo FMA contraction — see the header note).
            const simd::f32x8 dy_a  = simd::sub8(prev_ry, prev_th);  // > 0 (above)
            const simd::f32x8 dy_b  = simd::sub8(wy8, th8);          // <= 0 (below)
            const simd::f32x8 denom = simd::sub8(dy_a, dy_b);
            simd::f32x8       frac  = simd::div8(dy_a, denom);
            frac = simd::blend8(frac, zero, simd::cmp_le8(denom, zero));
            frac = simd::min8(simd::max8(frac, zero), simd::broadcast8(1.0f));

            const simd::f32x8 t_hit = simd::add8(prev_t, simd::mul8(simd::sub8(t8, prev_t), frac));
            res_t    = simd::blend8(res_t, t_hit, hit_now);
            hit_mask = simd::mask_or(hit_mask, hit_now);
            active   = simd::mask_and(active, simd::mask_not(hit_now));
            if (simd::none_of(active)) break;
        }

        prev_t  = t8;
        prev_ry = wy8;
        prev_th = th8;
        t += step_metres;
    }

    // Resolve hit positions: pos = origin + dir * t_hit, height re-sampled at
    // pos (matches `march_ray`, which re-samples bilinear at the hit point).
    const simd::f32x8 px = simd::add8(ox8, simd::mul8(dx8, res_t));
    const simd::f32x8 py = simd::add8(oy8, simd::mul8(dy8, res_t));
    const simd::f32x8 pz = simd::add8(oz8, simd::mul8(dz8, res_t));
    const simd::f32x8 ph = sample_bilinear_x8(h, px, pz);

    f32 a_t[8], a_px[8], a_py[8], a_pz[8], a_ph[8];
    simd::store_unaligned8(a_t, res_t);
    simd::store_unaligned8(a_px, px);
    simd::store_unaligned8(a_py, py);
    simd::store_unaligned8(a_pz, pz);
    simd::store_unaligned8(a_ph, ph);

    const int hm = simd::mask_to_int8(hit_mask);
    for (u32 i = 0; i < count && i < 8u; ++i) {
        if ((hm >> i) & 1) {
            out[i].hit    = true;
            out[i].t      = a_t[i];
            out[i].height = a_ph[i];
            out[i].pos    = math::Vec3{a_px[i], a_py[i], a_pz[i]};
        }
    }
}

// Job-parallel driver: march N independent rays against the heightfield. Each
// worker packs its slice into 8-wide packets (+ scalar-count tail packet).
// Serial below kRayParallelThreshold. `out` must be at least rays.size().
inline void march_rays(const HeightmapDesc& h, std::span<const HeightfieldRay> rays,
                       f32 step_metres, f32 max_t, std::span<RayMarchHit> out) noexcept {
    const usize n = rays.size();
    if (n == 0 || out.size() < n) return;

    const auto body = [&](usize begin, usize end) noexcept {
        usize i = begin;
        for (; i + 8u <= end; i += 8u) {
            march_packet8(h, &rays[i], 8u, step_metres, max_t, &out[i]);
        }
        if (i < end) {
            march_packet8(h, &rays[i], static_cast<u32>(end - i), step_metres, max_t, &out[i]);
        }
    };

    if (n < kRayParallelThreshold) {
        body(0, n);
    } else {
        jobs::JobSystem::Get().parallel_for(0, n, kRayGrain, body);
    }
}

// ─── CDLOD chunk LOD selection ────────────────────────────────────────────
// Selection metric: distance from the eye to the chunk's AABB centre. The
// per-vertex morph (cdlod_morph_t in CdlodMesh_internal.h) blends across the
// LOD band; this picks the discrete level. LOD 0 covers [0, leaf_range];
// each subsequent level doubles the range, capped at kMaxLodLevels - 1.
PSY_FORCEINLINE f32 chunk_center_distance(const math::Aabb& b, math::Vec3 eye) noexcept {
    const math::Vec3 c{(b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f,
                       (b.min.z + b.max.z) * 0.5f};
    return math::length(math::sub(c, eye));
}

PSY_FORCEINLINE u32 lod_for_distance(f32 dist, f32 leaf_range) noexcept {
    if (!(leaf_range > 0.0f)) return 0;
    u32 lod   = 0;
    f32 range = leaf_range;
    while (dist > range && lod + 1u < kMaxLodLevels) {
        range *= 2.0f;
        ++lod;
    }
    return lod;
}

// 8-wide LOD band selection. `dist8` → 8 LOD levels in `out`. The band count
// is computed branchlessly (cmp + blend), which reproduces the scalar
// `lod_for_distance` exactly: the predicate `dist > range` is monotonic as
// `range` doubles, so counting matching levels equals the scalar early-out
// loop. Levels 0..7 are exact in f32, so the float→u32 cast is lossless.
PSY_FORCEINLINE void select_lods_x8(simd::f32x8 dist8, f32 leaf_range, u32 out[8]) noexcept {
    if (!(leaf_range > 0.0f)) {
        for (int i = 0; i < 8; ++i) out[i] = 0;
        return;
    }
    const simd::f32x8 one   = simd::broadcast8(1.0f);
    simd::f32x8       lod8  = simd::broadcast8(0.0f);
    simd::f32x8       range = simd::broadcast8(leaf_range);
    for (u32 lvl = 0; lvl + 1u < kMaxLodLevels; ++lvl) {
        const simd::mask8 over = simd::cmp_gt8(dist8, range);
        lod8  = simd::blend8(lod8, simd::add8(lod8, one), over);
        range = simd::add8(range, range);  // *= 2
    }
    f32 lanes[8];
    simd::store_unaligned8(lanes, lod8);
    for (int i = 0; i < 8; ++i) out[i] = static_cast<u32>(lanes[i]);
}

// Job-parallel driver: pick a LOD per chunk for many chunk AABBs. Centres are
// gathered 8 at a time, distance is computed 8-wide (sqrt of the squared
// length, matching math::length), and `select_lods_x8` maps the band. Serial
// below kChunkLodParallelThreshold. `out_lods` must be at least bounds.size().
inline void select_chunk_lods(std::span<const math::Aabb> bounds, math::Vec3 eye, f32 leaf_range,
                              std::span<u8> out_lods) noexcept {
    const usize n = bounds.size();
    if (n == 0 || out_lods.size() < n) return;

    const auto body = [&](usize begin, usize end) noexcept {
        const simd::f32x8 ex = simd::broadcast8(eye.x);
        const simd::f32x8 ey = simd::broadcast8(eye.y);
        const simd::f32x8 ez = simd::broadcast8(eye.z);
        usize i = begin;
        for (; i + 8u <= end; i += 8u) {
            f32 cx[8], cy[8], cz[8];
            for (int k = 0; k < 8; ++k) {
                const math::Aabb& b = bounds[i + static_cast<usize>(k)];
                cx[k] = (b.min.x + b.max.x) * 0.5f;
                cy[k] = (b.min.y + b.max.y) * 0.5f;
                cz[k] = (b.min.z + b.max.z) * 0.5f;
            }
            const simd::f32x8 dx = simd::sub8(simd::load_unaligned8(cx), ex);
            const simd::f32x8 dy = simd::sub8(simd::load_unaligned8(cy), ey);
            const simd::f32x8 dz = simd::sub8(simd::load_unaligned8(cz), ez);
            const simd::f32x8 d2 =
                simd::add8(simd::add8(simd::mul8(dx, dx), simd::mul8(dy, dy)), simd::mul8(dz, dz));
            u32 lods[8];
            select_lods_x8(simd::sqrt8(d2), leaf_range, lods);
            for (int k = 0; k < 8; ++k) {
                out_lods[i + static_cast<usize>(k)] = static_cast<u8>(lods[k]);
            }
        }
        for (; i < end; ++i) {
            out_lods[i] =
                static_cast<u8>(lod_for_distance(chunk_center_distance(bounds[i], eye), leaf_range));
        }
    };

    if (n < kChunkLodParallelThreshold) {
        body(0, n);
    } else {
        jobs::JobSystem::Get().parallel_for(0, n, kChunkLodGrain, body);
    }
}

}  // namespace psynder::world::outdoor::detail
