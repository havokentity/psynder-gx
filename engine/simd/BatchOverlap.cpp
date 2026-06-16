// SPDX-License-Identifier: MIT
// Psynder — Lane 03 (SIMD). Batch broadphase-overlap kernel implementation.
//
// Mirrors the lane's established batched-routine shape (see Dispatch.cpp):
// one widest-path SIMD body guarded on __AVX__ + the runtime tier, then a
// scalar tail for the remainder. The f32x8 path is the primary width on
// AVX2 hosts; on Apple Silicon / SSE-only the f32x4 path carries the bulk
// and the same scalar tail closes it out.
//
// Determinism contract (see header): the per-lane squared distance is
//   d2 = (dx*dx + dy*dy) + dz*dz          // plain mul+add, fixed order
// and the predicate is `d2 <= r*r`. No FMA — fusing would change rounding
// and break bit-identity with the scalar reference. Every code path below
// computes that exact expression, so the SIMD mask equals the scalar mask
// bitwise for any input including the tail.

#include "BatchOverlap.h"

#include "Dispatch.h"
#include "Simd_internal.h"

#include "core/Types.h"

#include <algorithm>
#include <span>

namespace psynder::simd {

namespace {

// The single source of truth for "is point i inside?". One scalar lane of
// the kernel: identical arithmetic and association to the vector path, so
// the two agree bit-for-bit. Returns 1 when inside, 0 otherwise.
PSY_FORCEINLINE psynder::u8 inside_one(f32 cx, f32 cy, f32 cz, f32 r2,
                                       f32 px, f32 py, f32 pz) noexcept {
    const f32 dx = px - cx;
    const f32 dy = py - cy;
    const f32 dz = pz - cz;
    const f32 d2 = (dx * dx + dy * dy) + dz * dz;
    return d2 <= r2 ? psynder::u8{1} : psynder::u8{0};
}

// Common span-count: how many candidate points the coordinate streams
// jointly describe (SoA requires all three present for a point to exist).
PSY_FORCEINLINE psynder::usize point_count(std::span<const f32> xs,
                                           std::span<const f32> ys,
                                           std::span<const f32> zs) noexcept {
    return std::min(xs.size(), std::min(ys.size(), zs.size()));
}

}  // namespace

usize sphere_overlap_scalar(f32 cx, f32 cy, f32 cz, f32 r,
                            std::span<const f32> xs,
                            std::span<const f32> ys,
                            std::span<const f32> zs,
                            std::span<u8> out_inside) noexcept {
    const usize n   = std::min(point_count(xs, ys, zs), out_inside.size());
    const f32   r2  = r * r;
    usize       cnt = 0;
    for (usize i = 0; i < n; ++i) {
        const u8 hit  = inside_one(cx, cy, cz, r2, xs[i], ys[i], zs[i]);
        out_inside[i] = hit;
        cnt += hit;
    }
    return cnt;
}

usize sphere_overlap_mask(f32 cx, f32 cy, f32 cz, f32 r,
                          std::span<const f32> xs,
                          std::span<const f32> ys,
                          std::span<const f32> zs,
                          std::span<u8> out_inside) noexcept {
    const usize n  = std::min(point_count(xs, ys, zs), out_inside.size());
    const f32   r2 = r * r;

    const f32* px = xs.data();
    const f32* py = ys.data();
    const f32* pz = zs.data();
    u8*        po = out_inside.data();

    usize i   = 0;
    usize cnt = 0;

#if defined(__AVX__)
    // 8-wide widest path — only when the host actually carries AVX2/512 at
    // runtime (the binary may include the ops but a CPU without them would
    // #UD). Same guard the rest of the lane uses.
    if (current_tier() == Tier::Avx512 || current_tier() == Tier::Avx2) {
        const f32x8 vcx = broadcast8(cx);
        const f32x8 vcy = broadcast8(cy);
        const f32x8 vcz = broadcast8(cz);
        const f32x8 vr2 = broadcast8(r2);
        for (; i + 8 <= n; i += 8) {
            const f32x8 dx = sub8(load_unaligned8(px + i), vcx);
            const f32x8 dy = sub8(load_unaligned8(py + i), vcy);
            const f32x8 dz = sub8(load_unaligned8(pz + i), vcz);
            // (dx*dx + dy*dy) + dz*dz — plain mul/add, NOT fma. Matches the
            // scalar association exactly for bit-identity.
            const f32x8 d2 =
                add8(add8(mul8(dx, dx), mul8(dy, dy)), mul8(dz, dz));
            const mask8 m   = cmp_le8(d2, vr2);
            const int   bits = mask_to_int8(m);
            // Materialise the mask byte-per-lane and tally set bits.
            for (int lane = 0; lane < 8; ++lane) {
                const u8 hit = static_cast<u8>((bits >> lane) & 1);
                po[i + static_cast<usize>(lane)] = hit;
                cnt += hit;
            }
        }
    }
#endif

    // 4-wide path for the remaining whole groups (and the bulk on NEON /
    // SSE-only hosts where the 8-wide block above is skipped).
    {
        const f32x4 vcx = broadcast4(cx);
        const f32x4 vcy = broadcast4(cy);
        const f32x4 vcz = broadcast4(cz);
        const f32x4 vr2 = broadcast4(r2);
        for (; i + 4 <= n; i += 4) {
            const f32x4 dx = sub4(load_unaligned4(px + i), vcx);
            const f32x4 dy = sub4(load_unaligned4(py + i), vcy);
            const f32x4 dz = sub4(load_unaligned4(pz + i), vcz);
            const f32x4 d2 =
                add4(add4(mul4(dx, dx), mul4(dy, dy)), mul4(dz, dz));
            const mask4 m    = cmp_le4(d2, vr2);
            const int   bits = mask_to_int4(m);
            for (int lane = 0; lane < 4; ++lane) {
                const u8 hit = static_cast<u8>((bits >> lane) & 1);
                po[i + static_cast<usize>(lane)] = hit;
                cnt += hit;
            }
        }
    }

    // Scalar tail — the same inside_one() the reference uses, so the tail
    // bytes match the scalar mask exactly.
    for (; i < n; ++i) {
        const u8 hit = inside_one(cx, cy, cz, r2, px[i], py[i], pz[i]);
        po[i] = hit;
        cnt += hit;
    }
    return cnt;
}

usize sphere_overlap_indices(f32 cx, f32 cy, f32 cz, f32 r,
                             std::span<const f32> xs,
                             std::span<const f32> ys,
                             std::span<const f32> zs,
                             std::span<u32> out_indices) noexcept {
    const usize n   = point_count(xs, ys, zs);
    const usize cap = out_indices.size();
    const f32   r2  = r * r;

    usize written = 0;
    for (usize i = 0; i < n && written < cap; ++i) {
        if (inside_one(cx, cy, cz, r2, xs[i], ys[i], zs[i])) {
            out_indices[written++] = static_cast<u32>(i);
        }
    }
    return written;
}

}  // namespace psynder::simd
