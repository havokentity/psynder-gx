// SPDX-License-Identifier: MIT
// Scene-on-ECS migration, step 2: parsed scene primitives import into the ECS
// as canonical prop entities that render + physics queries consume. Builds the
// ParsedSceneDocument directly (the loose-scene *parser* is the editor's own,
// separately-tested concern) so this isolates the importer.

#include "../../samples/02_crate/SceneImport.h"

#include <catch2/catch_test_macros.hpp>

using psynder::math::Vec3;
using psynder::sample02::ParsedSceneDocument;
using psynder::sample02::ScenePrimitive;
using psynder::sample02::ScenePrimitiveKind;
using psynder::scene::Collider;
using psynder::scene::RenderMaterial;
using psynder::scene::ShapeKind;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

ScenePrimitive prim(const char* name, ScenePrimitiveKind kind, Vec3 pos, Vec3 scale) {
    ScenePrimitive p;
    p.name = name;
    p.kind = kind;
    p.position = pos;
    p.scale = scale;
    return p;
}

}  // namespace

TEST_CASE("scene/import: parsed primitives become canonical ECS prop entities",
          "[scene][ecs][import]") {
    ParsedSceneDocument doc;
    doc.primitives.push_back(prim("Floor", ScenePrimitiveKind::Plane, {0.0f, 0.0f, 0.0f},
                                  {10.0f, 1.0f, 10.0f}));
    ScenePrimitive crate_a = prim("CrateA", ScenePrimitiveKind::Cube, {2.0f, 0.5f, 0.0f},
                                  {1.0f, 1.0f, 1.0f});
    crate_a.material.albedo = {0.8f, 0.6f, 0.3f};
    doc.primitives.push_back(crate_a);
    doc.primitives.push_back(prim("CrateB", ScenePrimitiveKind::Cube, {-2.0f, 0.5f, 0.0f},
                                  {1.0f, 1.0f, 1.0f}));
    doc.primitives.push_back(prim("Ball", ScenePrimitiveKind::Sphere, {0.0f, 1.0f, 0.0f},
                                  {1.5f, 1.5f, 1.5f}));

    World w;
    const std::size_t spawned = psynder::sample02::import_scene_props(w, doc);
    REQUIRE(spawned == 4);

    std::size_t props = 0;
    std::size_t boxes = 0;
    std::size_t spheres = 0;
    std::size_t planes = 0;
    bool found_crate_a = false;
    w.for_each_chunk<TransformWS, Collider, RenderMaterial>(
        [&](std::size_t n, TransformWS* xf, Collider* col, RenderMaterial* mat) {
            for (std::size_t i = 0; i < n; ++i) {
                ++props;
                switch (col[i].kind) {
                    case ShapeKind::Box:     ++boxes; break;
                    case ShapeKind::Sphere:  ++spheres; break;
                    case ShapeKind::Plane:   ++planes; break;
                    case ShapeKind::Capsule: break;  // not produced by scene import
                }
                if (xf[i].mtw.m[12] == 2.0f) {  // CrateA, authored at x=+2
                    found_crate_a = true;
                    REQUIRE(col[i].kind == ShapeKind::Box);
                    REQUIRE(mat[i].albedo.x == 0.8f);
                    REQUIRE(xf[i].mtw.m[13] == 0.5f);
                }
            }
        });

    REQUIRE(props == 4);
    REQUIRE(boxes == 2);
    REQUIRE(spheres == 1);
    REQUIRE(planes == 1);
    REQUIRE(found_crate_a);

    // The sphere's collider radius comes from half the largest scale axis.
    bool checked_sphere = false;
    w.for_each_chunk<Collider>([&](std::size_t n, Collider* col) {
        for (std::size_t i = 0; i < n; ++i) {
            if (col[i].kind == ShapeKind::Sphere) {
                checked_sphere = true;
                REQUIRE(col[i].half_extents.x == 0.75f);  // 1.5 * 0.5
            }
        }
    });
    REQUIRE(checked_sphere);
}
