// SPDX-License-Identifier: MIT
// Psynder-GX — BSP face → BspMeshSet converter. Lane 12 owns; consumed by
// lane 09 (render-pipeline) which streams the per-cluster visible set into
// a GPU indirect-draw buffer.
//
// `lm_qbsp` writes BSP faces with material/lightmap IDs plus a flat
// (vertex, index) pool. The runtime is a *binding* pass — no vertex
// transformations, no material resolution — that emits one BspMeshSet per
// face. The caller threads through PVS / portal filtering before calling
// this converter.
//
// Wave-0 compat note: the former dependency on render/raster/Raster.h has
// been retired. All types are now GX-native (gpu::Handle<> handles, plain
// math types, u32 material ids). Lane 09 owns the actual GPU upload and
// indirect-draw encoding; this layer only shapes the CPU-side data.

#pragma once

#include <vector>  // Bsp.h uses std::vector without including <vector>; see Bsp.cpp.
#include "Bsp.h"
#include "gpu/PublicGpu.h"

#include <span>

namespace psynder::world::bsp {

// GX-native vertex layout matching the BSP geometry as lm_qbsp bakes it:
//   position(3) + normal(3) + uv(2) + lightmap_uv(2) + packed RGBA8 colour.
// Matches the vertex stride expected by lane 09's forward.slang input layout.
struct BspVertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec2 lightmap_uv;
    u32        color = 0xFFFFFFFFu; // packed RGBA8, default opaque white
};

// Per-face visible mesh record streamed to lane 09.
// `vertices` / `indices` are non-owning aliases into BspGeometry's vectors —
// BspGeometry MUST outlive any BspMeshSet that references it.
// Lane 09 uploads these into GPU vertex/index buffers and issues one
// indirect draw per entry (or batches by material for draw-call merging).
struct BspMeshSet {
    const BspVertex* vertices     = nullptr;
    u32              vertex_count = 0;
    const u32*       indices      = nullptr;
    u32              index_count  = 0;
    math::Mat4       model;         // identity for static BSP geometry
    u32              material_id  = 0; // opaque material handle (u32); lane 09
                                       // resolves against its material table
    u8               flags        = 0;
};

// Optional per-face overrides resolved from the asset system at level-load.
// Maps `BspFace::material` (an opaque id baked by lm_qbsp) onto the runtime
// material id resolved against the live material cache. A null override
// table means "pass the raw u32 through" (handy for tests).
struct BspMaterialResolve {
    const u32* table = nullptr;
    u32        count = 0;
};

// Per-BSP geometry tables. These live next to the BspMap and are filled at
// load time from the same .psybsp blob. Kept separate from `BspMap` so the
// public ABI in Bsp.h is preserved while we add geometry tables incrementally.
struct BspGeometry {
    std::vector<BspVertex> vertices;
    std::vector<u32>       indices;
};

// Build a BspMeshSet stream for every face in `faces`. `out` is appended to
// (so callers can accumulate across PVS callbacks). The BspMeshSet's
// `vertices` / `indices` pointers are aliases into `geom.vertices` /
// `geom.indices` — `geom` MUST outlive the resulting BspMeshSets.
void build_face_draws(const BspGeometry&          geom,
                      std::span<const BspFace>    faces,
                      const BspMaterialResolve&   resolve,
                      std::vector<BspMeshSet>&    out);

// Convenience: emit BspMeshSets for one leaf's face range. Used by the
// per-leaf callback in `walk_visible_leaves` (see Bsp.cpp).
void build_leaf_draws(const BspMap&               map,
                      const BspGeometry&          geom,
                      const BspLeaf&              leaf,
                      const BspMaterialResolve&   resolve,
                      std::vector<BspMeshSet>&    out);

}  // namespace psynder::world::bsp
