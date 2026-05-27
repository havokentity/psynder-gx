// SPDX-License-Identifier: MIT
// Scene-on-ECS migration, step 1: the canonical scene-runtime schema. A spawned
// prop must carry the components render (TransformWS + RenderMaterial) and
// physics (Collider) both query off the same entity.

#include "scene/SceneComponents.h"

#include <catch2/catch_test_macros.hpp>

using psynder::Entity;
using psynder::math::Vec3;
using psynder::scene::Collider;
using psynder::scene::PropDesc;
using psynder::scene::RenderMaterial;
using psynder::scene::ShapeKind;
using psynder::scene::TransformWS;
using psynder::scene::World;
using psynder::scene::spawn_prop;

TEST_CASE("scene/components: spawn_prop yields render+physics-queryable entities",
          "[scene][ecs][schema]") {
    World w;

    PropDesc crate;
    crate.position = {5.0f, 0.5f, -3.0f};
    crate.shape = ShapeKind::Box;
    crate.half_extents = {1.0f, 1.0f, 1.0f};
    crate.material.albedo = {0.8f, 0.6f, 0.3f};
    const Entity crate_e = spawn_prop(w, crate);

    PropDesc ball;
    ball.position = {0.0f, 2.0f, 0.0f};
    ball.shape = ShapeKind::Sphere;
    ball.half_extents = {0.5f, 0.5f, 0.5f};
    spawn_prop(w, ball);

    // One archetype query reaches both the render and physics components.
    std::size_t props = 0;
    w.for_each_chunk<TransformWS, Collider, RenderMaterial>(
        [&](std::size_t n, TransformWS*, Collider*, RenderMaterial*) { props += n; });
    REQUIRE(props == 2);

    // The transform's translation column carries the authored position.
    const TransformWS* xf = w.get<TransformWS>(crate_e);
    REQUIRE(xf != nullptr);
    REQUIRE(xf->mtw.m[12] == 5.0f);
    REQUIRE(xf->mtw.m[13] == 0.5f);
    REQUIRE(xf->mtw.m[14] == -3.0f);

    const Collider* col = w.get<Collider>(crate_e);
    REQUIRE(col != nullptr);
    REQUIRE(col->kind == ShapeKind::Box);
    REQUIRE(col->half_extents.x == 1.0f);

    const RenderMaterial* mat = w.get<RenderMaterial>(crate_e);
    REQUIRE(mat != nullptr);
    REQUIRE(mat->albedo.x == 0.8f);
}

TEST_CASE("scene/components: a sphere prop is distinguishable by shape kind",
          "[scene][ecs][schema]") {
    World w;
    PropDesc ball;
    ball.shape = ShapeKind::Sphere;
    const Entity e = spawn_prop(w, ball);
    REQUIRE(w.get<Collider>(e)->kind == ShapeKind::Sphere);
}
