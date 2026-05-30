// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Frustum.cpp
//
// Gribb-Hartmann frustum-plane extraction + conservative AABB / sphere culling.
// See Frustum.h for the full matrix-indexing convention and method notes.

#include "camera/Frustum.h"

#include <cmath>

namespace psynder::camera {

namespace {

// Normalize a plane in place so (nx, ny, nz) is unit length and `d` scales with
// it (so signed distance stays Euclidean). Exactly one sqrt — the only
// transcendental in this whole TU. A zero-length normal (degenerate / all-zero
// matrix) is left untouched rather than producing NaN/Inf.
void normalize_plane(Plane& p) noexcept {
    const f32 len2 = p.nx * p.nx + p.ny * p.ny + p.nz * p.nz;
    if (len2 > 0.0f) {
        const f32 inv = 1.0f / std::sqrt(len2);
        p.nx *= inv;
        p.ny *= inv;
        p.nz *= inv;
        p.d  *= inv;
    }
}

}  // namespace

Frustum frustum_from_view_proj(const math::Mat4& vp) noexcept {
    // Column-major storage: element (column c, row r) is m[c*4 + r]. The four
    // entries of matrix "row r" are m[r], m[4+r], m[8+r], m[12+r]. clip.i is the
    // dot of row i with the world vector (see math::mul(Mat4, Vec4)), so:
    const f32* m = vp.m;

    // Row 0 (clip.x):
    const f32 r0x = m[0], r0y = m[4], r0z = m[8],  r0w = m[12];
    // Row 1 (clip.y):
    const f32 r1x = m[1], r1y = m[5], r1z = m[9],  r1w = m[13];
    // Row 2 (clip.z):
    const f32 r2x = m[2], r2y = m[6], r2z = m[10], r2w = m[14];
    // Row 3 (clip.w):
    const f32 r3x = m[3], r3y = m[7], r3z = m[11], r3w = m[15];

    Frustum f{};

    // left = row3 + row0   (inside: x >= -w  ==>  w + x >= 0)
    f.planes[Frustum::kLeft]   = { r3x + r0x, r3y + r0y, r3z + r0z, r3w + r0w };
    // right = row3 - row0  (inside: x <=  w  ==>  w - x >= 0)
    f.planes[Frustum::kRight]  = { r3x - r0x, r3y - r0y, r3z - r0z, r3w - r0w };
    // bottom = row3 + row1 (inside: y >= -w)
    f.planes[Frustum::kBottom] = { r3x + r1x, r3y + r1y, r3z + r1z, r3w + r1w };
    // top = row3 - row1    (inside: y <=  w)
    f.planes[Frustum::kTop]    = { r3x - r1x, r3y - r1y, r3z - r1z, r3w - r1w };
    // nearp = row3 + row2  (inside: z >= -w  — RH / GL NDC z in [-1, 1])
    f.planes[Frustum::kNearp]  = { r3x + r2x, r3y + r2y, r3z + r2z, r3w + r2w };
    // farp = row3 - row2   (inside: z <=  w)
    f.planes[Frustum::kFarp]   = { r3x - r2x, r3y - r2y, r3z - r2z, r3w - r2w };

    for (u32 i = 0; i < Frustum::kCount; ++i) {
        normalize_plane(f.planes[i]);
    }
    return f;
}

bool aabb_in_frustum(const Frustum& f, math::Vec3 mn, math::Vec3 mx) noexcept {
    // p-vertex test: for each plane, pick the box corner farthest along the
    // plane normal (the "positive vertex"). If even that corner is behind the
    // plane, the whole box is outside this plane and thus the frustum.
    for (u32 i = 0; i < Frustum::kCount; ++i) {
        const Plane& p = f.planes[i];
        const f32 px = (p.nx >= 0.0f) ? mx.x : mn.x;
        const f32 py = (p.ny >= 0.0f) ? mx.y : mn.y;
        const f32 pz = (p.nz >= 0.0f) ? mx.z : mn.z;
        if (p.nx * px + p.ny * py + p.nz * pz + p.d < 0.0f) {
            return false;  // fully outside this plane
        }
    }
    return true;  // possibly visible
}

Cull classify_aabb(const Frustum& f, math::Vec3 mn, math::Vec3 mx) noexcept {
    bool intersecting = false;
    for (u32 i = 0; i < Frustum::kCount; ++i) {
        const Plane& p = f.planes[i];

        // p-vertex: farthest corner along +normal.
        const f32 px = (p.nx >= 0.0f) ? mx.x : mn.x;
        const f32 py = (p.ny >= 0.0f) ? mx.y : mn.y;
        const f32 pz = (p.nz >= 0.0f) ? mx.z : mn.z;
        if (p.nx * px + p.ny * py + p.nz * pz + p.d < 0.0f) {
            return Cull::Outside;  // even the best corner is behind — outside
        }

        // n-vertex: farthest corner along -normal (the opposite corner).
        const f32 nx = (p.nx >= 0.0f) ? mn.x : mx.x;
        const f32 ny = (p.ny >= 0.0f) ? mn.y : mx.y;
        const f32 nz = (p.nz >= 0.0f) ? mn.z : mx.z;
        if (p.nx * nx + p.ny * ny + p.nz * nz + p.d < 0.0f) {
            intersecting = true;  // worst corner behind this plane — straddles
        }
    }
    return intersecting ? Cull::Intersect : Cull::Inside;
}

bool sphere_in_frustum(const Frustum& f, math::Vec3 c, f32 radius) noexcept {
    // Normalized planes => signed distance is true Euclidean distance. The
    // sphere is fully outside a plane only when its center is more than `radius`
    // behind that plane.
    for (u32 i = 0; i < Frustum::kCount; ++i) {
        const Plane& p = f.planes[i];
        const f32 dist = p.nx * c.x + p.ny * c.y + p.nz * c.z + p.d;
        if (dist < -radius) {
            return false;
        }
    }
    return true;
}

}  // namespace psynder::camera
