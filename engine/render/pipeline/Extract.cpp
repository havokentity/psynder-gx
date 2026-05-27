// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/Extract.cpp
//
// Lane 09 — per-frame ECS extract (scene-on-ECS migration, step 3).
//
// Walks scene::World for prop entities carrying the canonical render schema
// (TransformWS + Collider + RenderMaterial — engine/scene/SceneComponents.h)
// and emits one Renderable per entity into the pipeline's preallocated draw
// list. The opaque pass then turns the list into draw_indexed calls (and,
// once lane 07 ships the indirect surface, GPU-driven indirect draws).
//
// DOTS contract (DESIGN §3 / AGENTS.md): iteration is via
// World::for_each_chunk over raw contiguous component columns — no
// per-entity virtual dispatch. The draw list is reserved at create() so the
// steady-state walk performs no heap allocation. Collider::kind selects one
// of the builtin unit meshes (box/sphere/plane); the entity's TransformWS
// world matrix carries position/rotation/scale into the draw.

#include "render/pipeline/Pipeline_internal.h"

#include "scene/SceneComponents.h"
#include "scene/World.h"

namespace psynder::render::pipeline {

namespace {

const BuiltinMesh& mesh_for(const Pipeline* p, scene::ShapeKind kind) {
    switch (kind) {
        case scene::ShapeKind::Sphere: return p->builtin_meshes.sphere;
        case scene::ShapeKind::Plane:  return p->builtin_meshes.plane;
        case scene::ShapeKind::Box:    break;
    }
    return p->builtin_meshes.box;
}

}  // namespace

void set_extract_world(Pipeline* p, scene::World* world) {
    if (p) p->extract_world = world;
}

void extract_renderables(Pipeline* p, const View& /*view*/) {
    if (!p) return;
    p->renderables.clear();

    scene::World& world = p->extract_world ? *p->extract_world : scene::World::Get();

    // DOTS walk: one chunk at a time, raw column pointers, no allocation.
    std::uint32_t material_id = 0;
    world.for_each_chunk<scene::TransformWS, scene::Collider, scene::RenderMaterial>(
        [&](std::size_t count,
            scene::TransformWS* xf,
            scene::Collider* col,
            scene::RenderMaterial* /*mat*/) {
            for (std::size_t i = 0; i < count; ++i) {
                const BuiltinMesh& mesh = mesh_for(p, col[i].kind);
                Renderable r{};
                r.vertex_buffer = mesh.vertex_buffer.get();
                r.index_buffer  = mesh.index_buffer.get();
                r.vertex_count  = mesh.vertex_count;
                r.index_count   = mesh.index_count;
                r.world_matrix  = xf[i].mtw;
                r.material_id   = material_id++;
                p->renderables.push_back(r);
            }
        });
}

}  // namespace psynder::render::pipeline
