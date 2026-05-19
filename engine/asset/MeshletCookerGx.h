// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/asset/MeshletCookerGx.h
//
// Wave A FROZEN public-header CONTRACT (lane 05-asset).
//
// Mesh-shader meshlet cooker. Takes a flat (vertex / index) buffer pair
// and emits a set of meshlets — clusters of ≤64 verts and ≤124 tris that
// can be dispatched by mesh shaders on supported hardware (Turing+,
// RDNA2+, M3+). On non-mesh-shader HW the renderer falls back to the
// vertex/index pipeline and ignores meshlet metadata.
//
// Per DESIGN-PSYNDER-GX.md §7.2 and §10.7: meshlets are pre-computed at
// cook time via meshoptimizer's `meshopt_buildMeshlets` algorithm (or
// similar). The cooker also writes per-meshlet bounding cones to enable
// HiZ-based per-meshlet culling in lane 09's compute cull pass.

#pragma once

#include "core/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace psynder::asset {

// ─── Input: a triangle mesh in flat vertex/index form ────────────────────
struct GxMeshSrcVertex {
    float position[3];
    float normal[3];
    float uv[2];
    // additional vertex streams (tangent, color, bone weights) are NOT
    // included in this view — only what the meshlet builder needs.
};

struct GxMeshSrc {
    std::span<const GxMeshSrcVertex> vertices;
    std::span<const std::uint32_t>   indices;
};

// ─── Output: meshlet metadata + cone bounds ──────────────────────────────
struct GxMeshletBounds {
    // Sphere bounds for the cluster (centre + radius), used for HiZ cull.
    float centre[3];
    float radius;
    // Backface cone (apex + axis + cone cos-angle) for cone backface cull.
    float cone_apex[3];
    float cone_axis[3];
    float cone_cutoff; // 1 = always pass; -1 = always cull
};

struct GxMeshlet {
    std::uint32_t vertex_offset;  // index into vertex_indices
    std::uint32_t vertex_count;   // ≤ 64
    std::uint32_t triangle_offset; // index into triangle_indices (×3 bytes)
    std::uint32_t triangle_count; // ≤ 124
    GxMeshletBounds bounds;
};

struct GxCookedMeshlets {
    // Indirection: meshlets index into vertex_indices + triangle_indices,
    // which in turn index into the source mesh's vertices.
    std::vector<GxMeshlet>      meshlets;
    std::vector<std::uint32_t>  vertex_indices;      // ≤ 64 per meshlet
    std::vector<std::uint8_t>   triangle_indices;    // packed 8-bit per-meshlet vertex idx
};

// ─── Cooker entry point (STUB — Wave B implementation) ───────────────────
//
// Wave B implementation will call `meshopt_buildMeshlets(...)` with
// max_vertices = 64, max_triangles = 124, cone_weight = 0.25 (typical
// values that maximize culling efficiency).
GxCookedMeshlets cook_meshlets(const GxMeshSrc&,
                               std::uint32_t max_vertices  = 64,
                               std::uint32_t max_triangles = 124,
                               float         cone_weight   = 0.25f);

// Convenience: emit a single "meshlet that covers the whole mesh" for
// dev/debug. Used by the renderer fallback when mesh shaders are off.
GxCookedMeshlets make_passthrough_meshlet(const GxMeshSrc&);

} // namespace psynder::asset
