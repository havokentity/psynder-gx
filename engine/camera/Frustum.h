// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Frustum.h
//
// Lane 16 — view-frustum extraction + AABB / sphere culling. The render
// pipeline's visibility primitive: turn a view-projection matrix into the six
// clip planes, then conservatively test world-space AABBs / spheres against
// them so the renderer can skip everything outside the camera's view volume.
//
// Method (Gribb-Hartmann, "Fast Extraction of Viewing Frustum Planes from the
// World-View-Projection Matrix"): each clip-space plane is a row-combination
// of the view-projection matrix. We normalize every plane so the stored normal
// is unit length and the plane's signed distance is true Euclidean distance —
// which lets `sphere_in_frustum` compare against the sphere radius directly.
//
// Matrix indexing convention (this engine, see engine/math/Math.h + Math.cpp):
//   - Mat4 stores 16 floats COLUMN-MAJOR: element at (column c, row r) lives at
//     `m[c*4 + r]`.
//   - A point is transformed by `math::mul(M, v)` (Math.cpp), which computes
//         clip.i = sum_c  M.m[c*4 + i] * v[c]
//     i.e. clip component `i` is the dot of MATRIX ROW i with the vector.
//     So:  clip.x = row 0,  clip.y = row 1,  clip.z = row 2,  clip.w = row 3.
//   - "Row r" of the matrix in this column-major layout is therefore the
//     four elements  { m[0*4+r], m[1*4+r], m[2*4+r], m[3*4+r] }
//                  = { m[r],     m[4+r],   m[8+r],   m[12+r]   }.
//   - Clip-space test for a point inside the frustum (RH, -Z forward, the
//     OpenGL-style w >= 0 volume):  -w <= x <= w,  -w <= y <= w,  -w <= z <= w.
//     Gribb-Hartmann turns each of those six inequalities into a plane whose
//     normal points INTO the frustum (positive signed distance == inside):
//         left   = row3 + row0      right = row3 - row0
//         bottom = row3 + row1      top   = row3 - row1
//         nearp  = row3 + row2      farp  = row3 - row2
//     This matches the engine's perspective_rh (NDC z in [-1, 1]) — the near
//     plane is row3 + row2, NOT the Direct3D-style bare row2.
//
// Determinism: same-platform strict-FP. Extraction is pure matrix algebra plus
// exactly one std::sqrt per plane for the normalize; no trig, no RNG. The same
// view-projection matrix yields bit-identical planes and bit-identical cull
// results across runs of the same platform / libm. This TU rides the camera
// lane's `-fno-fast-math -ffp-contract=off` (/fp:strict) build flags.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

namespace psynder::camera {

// ─── A single frustum plane ──────────────────────────────────────────────
// Stored as the normalized plane equation  nx*x + ny*y + nz*z + d = 0.
// The normal (nx, ny, nz) is UNIT length and points INTO the frustum, so the
// signed distance of a point P is  dot(n, P) + d  and is  >= 0  iff P is on the
// inside (visible) half-space of the plane.
struct Plane {
    f32 nx = 0.0f;
    f32 ny = 0.0f;
    f32 nz = 0.0f;
    f32 d  = 0.0f;
};

// ─── The six planes of a view frustum ────────────────────────────────────
// Index order is fixed and documented so callers can name planes:
//   [0] left   [1] right   [2] bottom   [3] top   [4] nearp   [5] farp
// (We deliberately avoid the identifiers `near` / `far` — they are macros in
//  some Windows SDK headers.)
struct Frustum {
    Plane planes[6];

    enum : u32 {
        kLeft   = 0,
        kRight  = 1,
        kBottom = 2,
        kTop    = 3,
        kNearp  = 4,
        kFarp   = 5,
        kCount  = 6,
    };
};

// ─── AABB classification result ──────────────────────────────────────────
enum class Cull : u32 {
    Outside   = 0,   // fully outside at least one plane — definitely culled
    Intersect = 1,   // straddles the boundary — partially visible
    Inside    = 2,   // fully inside all six planes — trivially accept
};

// Extract + normalize the six frustum planes from a view-projection matrix.
// `view_proj` must be the composed matrix such that  clip = math::mul(view_proj,
// world)  (e.g. camera::view_proj_matrix(cam, aspect), or
// math::mul(perspective_rh(...), look_at_rh(...))). See the header note above
// for the row/column indexing this uses on the engine's column-major m[16].
//
// A degenerate plane (zero-length normal, e.g. from an all-zero matrix) is left
// un-normalized rather than producing NaN; callers should pass a real
// view-projection matrix.
Frustum frustum_from_view_proj(const math::Mat4& view_proj) noexcept;

// Conservative AABB visibility (the standard "p-vertex" test). Returns false
// iff the box lies fully outside at least one plane (definitely not visible);
// returns true if the box is possibly visible. `min`/`max` are the world-space
// corner extents (componentwise min <= max expected; the test still works if a
// caller passes them swapped on an axis, it just becomes less tight).
bool aabb_in_frustum(const Frustum& f, math::Vec3 min, math::Vec3 max) noexcept;

// Full Inside / Intersect / Outside classification via the p-vertex (positive
// vertex, farthest along +n) and n-vertex (negative vertex, farthest along -n)
// tests. Outside if the p-vertex is behind any plane; Inside if even the
// n-vertex is in front of every plane; otherwise Intersect.
Cull classify_aabb(const Frustum& f, math::Vec3 min, math::Vec3 max) noexcept;

// Sphere visibility: true iff the sphere's signed distance to every plane is
// >= -radius (i.e. the sphere is not fully behind any plane). Because the
// planes are normalized, the signed distance is true Euclidean distance.
bool sphere_in_frustum(const Frustum& f, math::Vec3 center, f32 radius) noexcept;

}  // namespace psynder::camera
