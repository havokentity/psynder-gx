// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/VolumetricSmokeField.h
//
// Lane 08 — GPU volumetric smoke (ADR-022 / issue #44), CPU-side scaffold.
//
// This is the DETERMINISTIC coarse density field that drives gameplay
// line-of-sight: a fixed-resolution 3D voxel grid of smoke density. The
// gameplay/netcode layer queries sample() to decide whether a sightline is
// occluded, so the field MUST be bit-fair across clients (lockstep replay).
//
// The eye-candy raymarch — injecting/advecting density on the GPU and
// raymarching it in a fragment/compute pass — is a separate, later piece of
// work that consumes this same authoritative coarse field for occlusion.
//
// TODO(ADR-022/#44): GPU compute injection/advection + raymarch in psy::gpu;
// this CPU grid is the authoritative coarse LOS field.
//
// DOTS contract (DESIGN §3,§14):
//   * POD, SoA: density lives in ONE contiguous std::vector<f32>, flat-indexed
//     z*W*H + y*W + x. No per-cell objects, no per-frame heap allocation —
//     the array is sized once at construction and only its contents mutate.
//   * No exceptions / RTTI in the hot path (inject/step/carve/sample).
//   * Determinism: stable left-to-right voxel iteration, no wall-clock, no
//     fast-math reassociation (the lane builds with -fno-fast-math). 1u = 1m.
//
// Header-only so the unit test compiles against it without linking the full
// psynder_shader lib (which pulls in slang/gpu). The grid is plain math.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <vector>

namespace psynder::shader {

// Fixed-resolution axis-aligned smoke density grid in world space.
//
// World->voxel mapping: voxel (i,j,k) spans the metric cube
//   [origin + (i,j,k)*cell_m , origin + (i+1,j+1,k+1)*cell_m)
// and its sample point (cell centre) is origin + (i+0.5,j+0.5,k+0.5)*cell_m.
//
// Density is a unitless scalar in [0, +inf) clamped to [0,1] on write; 0 is
// clear air, 1 is fully opaque smoke. inject() accumulates, step() decays,
// carve() punches a clear sphere (a bullet hole through the cloud).
class VolumetricSmokeField {
public:
    using Vec3 = math::Vec3;

    // Construct a grid of `dim_x * dim_y * dim_z` voxels, each `cell_m` metres
    // on a side, with its (0,0,0) corner at `origin_m`. All density starts 0.
    // `cell_m` must be > 0; non-positive falls back to 1 metre so the mapping
    // never divides by zero (no exception in this POD-style ctor).
    VolumetricSmokeField(u32 dim_x, u32 dim_y, u32 dim_z,
                         f32 cell_m, Vec3 origin_m) noexcept
        : dim_x_(dim_x ? dim_x : 1u),
          dim_y_(dim_y ? dim_y : 1u),
          dim_z_(dim_z ? dim_z : 1u),
          cell_m_(cell_m > 0.0f ? cell_m : 1.0f),
          inv_cell_m_(1.0f / (cell_m > 0.0f ? cell_m : 1.0f)),
          origin_m_(origin_m),
          density_(static_cast<usize>(dim_x_) * dim_y_ * dim_z_, 0.0f) {}

    // ─── Dimensions / mapping ────────────────────────────────────────────
    u32   dim_x()  const noexcept { return dim_x_; }
    u32   dim_y()  const noexcept { return dim_y_; }
    u32   dim_z()  const noexcept { return dim_z_; }
    f32   cell_m() const noexcept { return cell_m_; }
    Vec3  origin_m() const noexcept { return origin_m_; }
    usize voxel_count() const noexcept { return density_.size(); }

    // Flat SoA index of voxel (x,y,z). Layout: z*W*H + y*W + x.
    usize voxel_index(u32 x, u32 y, u32 z) const noexcept {
        return (static_cast<usize>(z) * dim_y_ + y) * dim_x_ + x;
    }

    // Map a world position (metres) to the integer voxel that contains it,
    // floored, then clamped to the grid bounds. Returns the clamped voxel and
    // whether the raw (pre-clamp) point was actually inside the grid.
    struct VoxelCoord { u32 x, y, z; bool inside; };
    VoxelCoord world_to_voxel(Vec3 pos_m) const noexcept {
        const f32 fx = (pos_m.x - origin_m_.x) * inv_cell_m_;
        const f32 fy = (pos_m.y - origin_m_.y) * inv_cell_m_;
        const f32 fz = (pos_m.z - origin_m_.z) * inv_cell_m_;
        const i32 ix = floor_i32(fx);
        const i32 iy = floor_i32(fy);
        const i32 iz = floor_i32(fz);
        const bool inside =
            ix >= 0 && iy >= 0 && iz >= 0 &&
            ix < static_cast<i32>(dim_x_) &&
            iy < static_cast<i32>(dim_y_) &&
            iz < static_cast<i32>(dim_z_);
        return {clamp_axis(ix, dim_x_), clamp_axis(iy, dim_y_),
                clamp_axis(iz, dim_z_), inside};
    }

    // World-space centre of voxel (x,y,z).
    Vec3 voxel_center_m(u32 x, u32 y, u32 z) const noexcept {
        return Vec3{origin_m_.x + (static_cast<f32>(x) + 0.5f) * cell_m_,
                    origin_m_.y + (static_cast<f32>(y) + 0.5f) * cell_m_,
                    origin_m_.z + (static_cast<f32>(z) + 0.5f) * cell_m_};
    }

    // ─── Mutators (hot path) ─────────────────────────────────────────────

    // Add `amount` density to every voxel whose centre lies within `radius_m`
    // of `center_m`, falling off smoothly to 0 at the rim (1 - (d/r)^2). The
    // result is clamped to [0,1]. Iterates only the AABB of the sphere; voxel
    // order is the deterministic z-outer / x-inner triple loop.
    void inject(Vec3 center_m, f32 radius_m, f32 amount) noexcept {
        if (radius_m <= 0.0f || amount == 0.0f) return;
        const f32 r2 = radius_m * radius_m;
        forEachVoxelInSphere(center_m, radius_m,
            [&](usize idx, f32 d2) noexcept {
                const f32 falloff = 1.0f - d2 / r2;       // 1 at centre → 0 at rim
                f32 v = density_[idx] + amount * falloff;
                density_[idx] = clamp01(v);
            });
    }

    // Decay the whole field toward 0 by multiplying every voxel by
    // (1 - dissipation). dissipation is clamped to [0,1]; 0 holds the field,
    // 1 clears it. Deterministic flat sweep over the contiguous array.
    void step(f32 dissipation) noexcept {
        const f32 keep = 1.0f - clamp01(dissipation);
        for (usize i = 0, n = density_.size(); i < n; ++i) {
            density_[i] *= keep;
        }
    }

    // Zero density inside a sphere — a bullet hole carved through the cloud.
    void carve(Vec3 center_m, f32 radius_m) noexcept {
        if (radius_m <= 0.0f) return;
        forEachVoxelInSphere(center_m, radius_m,
            [&](usize idx, f32 /*d2*/) noexcept { density_[idx] = 0.0f; });
    }

    // ─── Sampling ────────────────────────────────────────────────────────

    // Trilinear density at a world position. Positions outside the grid read
    // as 0 (clear air). Uses the cell-centre sample lattice; samples that
    // straddle the boundary clamp the far edge to the border voxel.
    f32 sample(Vec3 pos_m) const noexcept {
        // Continuous voxel-centre coordinates: centre of voxel i sits at i.
        const f32 gx = (pos_m.x - origin_m_.x) * inv_cell_m_ - 0.5f;
        const f32 gy = (pos_m.y - origin_m_.y) * inv_cell_m_ - 0.5f;
        const f32 gz = (pos_m.z - origin_m_.z) * inv_cell_m_ - 0.5f;

        const i32 x0 = floor_i32(gx);
        const i32 y0 = floor_i32(gy);
        const i32 z0 = floor_i32(gz);

        const f32 tx = gx - static_cast<f32>(x0);
        const f32 ty = gy - static_cast<f32>(y0);
        const f32 tz = gz - static_cast<f32>(z0);

        // Eight corner reads (clamped to border); a corner fully outside the
        // grid still clamps in, but the lerp weight pulls it toward the edge.
        const f32 c000 = at(x0,     y0,     z0);
        const f32 c100 = at(x0 + 1, y0,     z0);
        const f32 c010 = at(x0,     y0 + 1, z0);
        const f32 c110 = at(x0 + 1, y0 + 1, z0);
        const f32 c001 = at(x0,     y0,     z0 + 1);
        const f32 c101 = at(x0 + 1, y0,     z0 + 1);
        const f32 c011 = at(x0,     y0 + 1, z0 + 1);
        const f32 c111 = at(x0 + 1, y0 + 1, z0 + 1);

        const f32 c00 = lerp(c000, c100, tx);
        const f32 c10 = lerp(c010, c110, tx);
        const f32 c01 = lerp(c001, c101, tx);
        const f32 c11 = lerp(c011, c111, tx);
        const f32 c0  = lerp(c00, c10, ty);
        const f32 c1  = lerp(c01, c11, ty);
        return lerp(c0, c1, tz);
    }

    // Nearest-voxel density at a world position (0 outside the grid). Cheaper
    // LOS probe than the trilinear sample().
    f32 sample_nearest(Vec3 pos_m) const noexcept {
        const VoxelCoord c = world_to_voxel(pos_m);
        if (!c.inside) return 0.0f;
        return density_[voxel_index(c.x, c.y, c.z)];
    }

    // Raw density at integer voxel coords clamped to the border (0 if grid is
    // somehow empty — never happens post-ctor). Public for tests/tools.
    f32 at(i32 x, i32 y, i32 z) const noexcept {
        if (density_.empty()) return 0.0f;
        const u32 cx = clamp_axis(x, dim_x_);
        const u32 cy = clamp_axis(y, dim_y_);
        const u32 cz = clamp_axis(z, dim_z_);
        return density_[voxel_index(cx, cy, cz)];
    }

    // Read-only view of the flat SoA density array (for tests / GPU upload).
    const std::vector<f32>& data() const noexcept { return density_; }

private:
    // Deterministic iteration over voxels whose CENTRE lies within radius of
    // center_m. Visits only the clamped AABB of the sphere, z-outer x-inner.
    template <class Fn>
    void forEachVoxelInSphere(Vec3 center_m, f32 radius_m, Fn&& fn) noexcept {
        const f32 r2 = radius_m * radius_m;
        // Sphere AABB in continuous voxel space, then clamp to the grid.
        const f32 cx = (center_m.x - origin_m_.x) * inv_cell_m_;
        const f32 cy = (center_m.y - origin_m_.y) * inv_cell_m_;
        const f32 cz = (center_m.z - origin_m_.z) * inv_cell_m_;
        const f32 rv = radius_m * inv_cell_m_;

        const u32 x_lo = clamp_axis(floor_i32(cx - rv), dim_x_);
        const u32 x_hi = clamp_axis(floor_i32(cx + rv), dim_x_);
        const u32 y_lo = clamp_axis(floor_i32(cy - rv), dim_y_);
        const u32 y_hi = clamp_axis(floor_i32(cy + rv), dim_y_);
        const u32 z_lo = clamp_axis(floor_i32(cz - rv), dim_z_);
        const u32 z_hi = clamp_axis(floor_i32(cz + rv), dim_z_);

        for (u32 z = z_lo; z <= z_hi; ++z) {
            for (u32 y = y_lo; y <= y_hi; ++y) {
                for (u32 x = x_lo; x <= x_hi; ++x) {
                    const Vec3 c = voxel_center_m(x, y, z);
                    const f32 dx = c.x - center_m.x;
                    const f32 dy = c.y - center_m.y;
                    const f32 dz = c.z - center_m.z;
                    const f32 d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 <= r2) fn(voxel_index(x, y, z), d2);
                }
            }
        }
    }

    static f32 clamp01(f32 v) noexcept {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    static f32 lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }

    // floor() to i32 without pulling in <cmath> branches in the hot loop;
    // deterministic for the finite values we feed it.
    static i32 floor_i32(f32 v) noexcept {
        const i32 i = static_cast<i32>(v);
        return (v < static_cast<f32>(i)) ? i - 1 : i;
    }

    // Clamp a (possibly out-of-range) axis index into [0, dim-1].
    static u32 clamp_axis(i32 v, u32 dim) noexcept {
        if (v < 0) return 0u;
        const u32 hi = dim - 1u;
        return static_cast<u32>(v) > hi ? hi : static_cast<u32>(v);
    }

    u32  dim_x_, dim_y_, dim_z_;
    f32  cell_m_;
    f32  inv_cell_m_;
    Vec3 origin_m_;
    // SoA: one contiguous density array, sized once at construction. No
    // per-cell allocation; mutators only touch the floats in place.
    std::vector<f32> density_;
};

} // namespace psynder::shader
