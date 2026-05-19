// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/Extract.cpp
//
// Lane 09 — per-frame ECS extract.
//
// Walks the World (lane 06) to collect renderable entities and pushes
// them into Pipeline::renderables. The render() entry point then turns
// the list into draw indirect arguments through the cull compute pass.
//
// Wave-B scope (M1): the World query for visible renderables isn't yet
// schema-stable across lanes (lane 06's GxComponents header is not
// present at the time of this commit — see INTEGRATION.txt for the
// expected component shape and the cross-lane Issue lane09-003). To keep
// the M1 demo path functional regardless, this extract:
//
//   1. Drops the previous frame's list.
//   2. If the Pipeline has an M1 triangle uploaded, pushes one
//      Renderable referencing those buffers with an identity transform.
//   3. (M2+) walks lane 06 World for entities with a MeshRef +
//      TransformWS component, frustum-culls on CPU as a sanity check,
//      and emits one Renderable per visible entity. The GPU cull pass
//      then narrows further via HiZ occlusion.

#include "render/pipeline/Pipeline_internal.h"

#include "scene/World.h"

namespace psynder::render::pipeline {

void extract_renderables(Pipeline* p, const View& /*view*/) {
    if (!p) return;
    p->renderables.clear();

    // ─── M1 triangle path ───────────────────────────────────────────────
    if (p->m1_triangle.vertex_buffer && p->m1_triangle.index_buffer) {
        Renderable r{};
        r.vertex_buffer = p->m1_triangle.vertex_buffer.get();
        r.index_buffer  = p->m1_triangle.index_buffer.get();
        r.vertex_count  = p->m1_triangle.vertex_count;
        r.index_count   = p->m1_triangle.index_count;
        // Identity world matrix (triangle is already in clip space).
        // Stored in column-major: a Mat4 with diag = 1.
        r.world_matrix.m[0]  = 1.0f;
        r.world_matrix.m[5]  = 1.0f;
        r.world_matrix.m[10] = 1.0f;
        r.world_matrix.m[15] = 1.0f;
        r.material_id   = 0; // M1 uses the passthrough pipeline (no material)
        p->renderables.push_back(r);
    }

    // ─── M2+ ECS walk (scaffold) ────────────────────────────────────────
    //
    // Pseudocode for when lane 06's GxComponents lands:
    //
    //   auto& world = scene::World::Get();
    //   world.query<reads<TransformWS, MeshRef, MaterialRef>>([&](Entity e,
    //       const TransformWS& xf, const MeshRef& mref, const MaterialRef& mat)
    //   {
    //       if (!frustum_intersects(view, mesh_aabb_ws(mref, xf))) return;
    //       Renderable r{};
    //       r.vertex_buffer = mesh_pool.vb_for(mref.mesh_id);
    //       r.index_buffer  = mesh_pool.ib_for(mref.mesh_id);
    //       r.vertex_count  = mesh_pool.vc_for(mref.mesh_id);
    //       r.index_count   = mesh_pool.ic_for(mref.mesh_id);
    //       r.world_matrix  = xf.matrix;
    //       r.material_id   = mat.id;
    //       p->renderables.push_back(r);
    //   });
    //
    // The MeshRef / MaterialRef / TransformWS component schema is owned
    // by lane 06 (GxComponents.h). Tracking issue: lane09-003 against
    // lane 06 to publish the canonical schema headers before M2 lands.
    //
    // For Wave-B M1 the M1 triangle path above is the entire content of
    // the renderables list, which is sufficient for the M1 acceptance
    // gate per PLAN.md §3.
    (void)scene::World::Get; // keep the include referenced
}

} // namespace psynder::render::pipeline
