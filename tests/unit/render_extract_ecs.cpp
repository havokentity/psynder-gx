// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/render_extract_ecs.cpp
//
// Scene-on-ECS migration, step 3: the render extract walks scene::World for
// prop entities (TransformWS + Collider + RenderMaterial) and emits one
// Renderable per entity, carrying the entity's world matrix. Verified
// headless (no GPU device) — builtin mesh buffers are null, but the
// extracted count + world matrices are fully determined by the ECS.

#include "render/pipeline/PublicRenderPipeline.h"
#include "render/pipeline/Pipeline_internal.h"

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace psy_rp = psynder::render::pipeline;

namespace psynder::render::pipeline {
void set_device_hook(psynder::gpu::Device*);
}

namespace {

psy_rp::PipelineDesc make_desc() {
    psy_rp::PipelineDesc d{};
    d.internal_width   = 1280;
    d.internal_height  = 720;
    d.light_clusters_x = 16;
    d.light_clusters_y = 9;
    d.light_clusters_z = 24;
    return d;
}

}  // namespace

TEST_CASE("render-extract: walks scene::World props into the draw list",
          "[render-pipeline][ecs][extract]") {
    psynder::render::pipeline::set_device_hook(nullptr);  // headless

    psynder::scene::World w;

    psynder::scene::PropDesc crate;
    crate.position = {5.0f, 0.5f, -3.0f};
    crate.shape = psynder::scene::ShapeKind::Box;
    const psynder::Entity crate_e = psynder::scene::spawn_prop(w, crate);

    psynder::scene::PropDesc ball;
    ball.position = {-2.0f, 2.0f, 1.0f};
    ball.shape = psynder::scene::ShapeKind::Sphere;
    psynder::scene::spawn_prop(w, ball);

    psynder::scene::PropDesc floor;
    floor.position = {0.0f, 0.0f, 0.0f};
    floor.shape = psynder::scene::ShapeKind::Plane;
    psynder::scene::spawn_prop(w, floor);

    auto desc = make_desc();
    auto* p = psy_rp::create(desc);
    REQUIRE(p != nullptr);

    psy_rp::set_extract_world(p, &w);

    psy_rp::View view{};
    psy_rp::extract_renderables(p, view);

    // One renderable per spawned prop.
    REQUIRE(p->renderables.size() == 3);

    // The crate's renderable carries its authored translation (mtw's last
    // column). Find it by matching the translation we authored.
    bool found_crate = false;
    for (const auto& r : p->renderables) {
        if (r.world_matrix.m[12] == 5.0f &&
            r.world_matrix.m[13] == 0.5f &&
            r.world_matrix.m[14] == -3.0f) {
            found_crate = true;
        }
    }
    REQUIRE(found_crate);

    // Sanity: the ECS world matrix equals the entity's TransformWS.
    const auto* xf = w.get<psynder::scene::TransformWS>(crate_e);
    REQUIRE(xf != nullptr);
    for (const auto& r : p->renderables) {
        if (r.world_matrix.m[12] == 5.0f) {
            for (int i = 0; i < 16; ++i) {
                REQUIRE(r.world_matrix.m[i] == xf->mtw.m[i]);
            }
        }
    }

    // Re-running the extract clears + refills (no accumulation).
    psy_rp::extract_renderables(p, view);
    REQUIRE(p->renderables.size() == 3);

    psy_rp::destroy(p);
    psynder::render::pipeline::set_device_hook(nullptr);
}

TEST_CASE("render-extract: empty world yields an empty draw list",
          "[render-pipeline][ecs][extract]") {
    psynder::render::pipeline::set_device_hook(nullptr);

    psynder::scene::World w;
    auto desc = make_desc();
    auto* p = psy_rp::create(desc);
    REQUIRE(p != nullptr);

    psy_rp::set_extract_world(p, &w);
    psy_rp::View view{};
    psy_rp::extract_renderables(p, view);
    REQUIRE(p->renderables.empty());

    psy_rp::destroy(p);
}
