// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Round-trips a scene::World through SceneSerialize: a save/load preserves the
// entity count, every component's values (matrix translation within Approx,
// integer fields exact), rejects malformed buffers, and produces deterministic
// (byte-identical) output for an identical scene.

#include "scene/SceneComponents.h"
#include "scene/SceneSerialize.h"
#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

using psynder::Entity;
using psynder::u8;
using psynder::scene::Collider;
using psynder::scene::DynamicBody;
using psynder::scene::RenderMaterial;
using psynder::scene::ShapeKind;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

// Build a small authored scene: three props plus one dynamic prop, each with a
// distinct translation so we can tell them apart after a reload.
void build_scene(World& w) {
    using psynder::scene::PropDesc;
    using psynder::scene::DynamicPropDesc;

    PropDesc a;
    a.position = {1.0f, 2.0f, 3.0f};
    a.shape = ShapeKind::Box;
    a.half_extents = {0.5f, 0.5f, 0.5f};
    a.material = {{0.1f, 0.2f, 0.3f}, 0.4f, 0.0f, {0.0f, 0.0f, 0.0f}, 0.0f};
    psynder::scene::spawn_prop(w, a);

    PropDesc b;
    b.position = {-7.0f, 0.25f, 12.5f};
    b.shape = ShapeKind::Sphere;
    b.half_extents = {1.25f, 1.25f, 1.25f};
    b.material = {{0.9f, 0.8f, 0.7f}, 0.65f, 1.0f, {2.0f, 0.0f, 0.0f}, 5.0f};
    psynder::scene::spawn_prop(w, b);

    PropDesc c;
    c.position = {100.0f, -50.0f, 0.0f};
    c.shape = ShapeKind::Plane;
    c.half_extents = {50.0f, 0.0f, 50.0f};
    psynder::scene::spawn_prop(w, c);

    DynamicPropDesc d;
    d.position = {0.0f, 9.0f, 0.0f};
    d.shape = ShapeKind::Capsule;
    d.half_extents = {0.4f, 0.9f, 0.4f};
    d.mass_kg = 8.0f;
    d.friction = 0.5f;
    d.restitution = 0.1f;
    psynder::scene::spawn_dynamic_prop(w, d);
}

// Pull translation (last column of a column-major mat4) out of a transform.
struct Translation { float x, y, z; };
Translation translation_of(const TransformWS& t) {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}

std::size_t count_transforms(World& w) {
    std::size_t n = 0;
    w.for_each_chunk<TransformWS>([&](std::size_t k, TransformWS*) { n += k; });
    return n;
}

}  // namespace

TEST_CASE("scene/serialize: round-trips entities and component values", "[scene]") {
    World src;
    build_scene(src);
    const std::size_t original_count = count_transforms(src);
    REQUIRE(original_count == 4);

    std::vector<u8> blob;
    psynder::scene::serialize_scene(src, blob);
    REQUIRE_FALSE(blob.empty());

    World dst;
    REQUIRE(psynder::scene::deserialize_scene(blob, dst));
    REQUIRE(count_transforms(dst) == original_count);

    // Match loaded entities back to source by translation. Each source prop has
    // a unique translation, so we can pair them up and compare every field.
    std::size_t matched = 0;
    src.for_each_chunk_with_entities<TransformWS>(
        [&](std::size_t sn, const Entity* se, TransformWS* sx) {
            for (std::size_t i = 0; i < sn; ++i) {
                const Translation st = translation_of(sx[i]);
                const Collider* scol = src.get<Collider>(se[i]);
                const RenderMaterial* smat = src.get<RenderMaterial>(se[i]);
                const DynamicBody* sbody = src.get<DynamicBody>(se[i]);

                bool found = false;
                dst.for_each_chunk_with_entities<TransformWS>(
                    [&](std::size_t dn, const Entity* de, TransformWS* dx) {
                        for (std::size_t j = 0; j < dn; ++j) {
                            const Translation dt = translation_of(dx[j]);
                            const float eps = 1e-6f;
                            if (std::fabs(dt.x - st.x) > eps ||
                                std::fabs(dt.y - st.y) > eps ||
                                std::fabs(dt.z - st.z) > eps) {
                                continue;
                            }
                            found = true;

                            // Translation matches within Approx.
                            CHECK(dt.x == Catch::Approx(st.x));
                            CHECK(dt.y == Catch::Approx(st.y));
                            CHECK(dt.z == Catch::Approx(st.z));

                            // Full 32-float matrix bit-identical (exact reload).
                            for (int k = 0; k < 16; ++k) {
                                CHECK(dx[j].mtw.m[k] == sx[i].mtw.m[k]);
                                CHECK(dx[j].prev_mtw.m[k] == sx[i].prev_mtw.m[k]);
                            }

                            const Collider* dcol = dst.get<Collider>(de[j]);
                            REQUIRE((scol != nullptr) == (dcol != nullptr));
                            if (scol && dcol) {
                                // ShapeKind is an integer field — exact.
                                CHECK(static_cast<unsigned>(dcol->kind) ==
                                      static_cast<unsigned>(scol->kind));
                                CHECK(dcol->half_extents.x == scol->half_extents.x);
                                CHECK(dcol->half_extents.y == scol->half_extents.y);
                                CHECK(dcol->half_extents.z == scol->half_extents.z);
                            }

                            const RenderMaterial* dmat = dst.get<RenderMaterial>(de[j]);
                            REQUIRE((smat != nullptr) == (dmat != nullptr));
                            if (smat && dmat) {
                                CHECK(dmat->albedo.x == smat->albedo.x);
                                CHECK(dmat->albedo.y == smat->albedo.y);
                                CHECK(dmat->albedo.z == smat->albedo.z);
                                CHECK(dmat->roughness == smat->roughness);
                                CHECK(dmat->metallic == smat->metallic);
                                CHECK(dmat->emissive.x == smat->emissive.x);
                                CHECK(dmat->emissive.y == smat->emissive.y);
                                CHECK(dmat->emissive.z == smat->emissive.z);
                                CHECK(dmat->emissive_intensity == smat->emissive_intensity);
                            }

                            const DynamicBody* dbody = dst.get<DynamicBody>(de[j]);
                            REQUIRE((sbody != nullptr) == (dbody != nullptr));
                            if (sbody && dbody) {
                                CHECK(dbody->mass_kg == sbody->mass_kg);
                                CHECK(dbody->friction == sbody->friction);
                                CHECK(dbody->restitution == sbody->restitution);
                            }
                            return;
                        }
                    });
                CHECK(found);
                if (found) ++matched;
            }
        });
    CHECK(matched == original_count);
}

TEST_CASE("scene/serialize: rejects malformed and truncated buffers", "[scene]") {
    World src;
    build_scene(src);
    std::vector<u8> blob;
    psynder::scene::serialize_scene(src, blob);
    REQUIRE(blob.size() > 12);

    // Empty buffer — no header.
    {
        World dst;
        std::vector<u8> empty;
        CHECK_FALSE(psynder::scene::deserialize_scene(empty, dst));
    }

    // Bad magic — flip the first byte.
    {
        World dst;
        std::vector<u8> bad = blob;
        bad[0] ^= 0xFFu;
        CHECK_FALSE(psynder::scene::deserialize_scene(bad, dst));
    }

    // Truncated body — header claims N entities but the record bytes are cut.
    {
        World dst;
        std::vector<u8> truncated(blob.begin(), blob.begin() + 20);
        CHECK_FALSE(psynder::scene::deserialize_scene(truncated, dst));
    }

    // Garbage — random bytes with no valid header.
    {
        World dst;
        std::vector<u8> garbage(64, 0xABu);
        CHECK_FALSE(psynder::scene::deserialize_scene(garbage, dst));
    }
}

TEST_CASE("scene/serialize: identical scenes produce identical bytes", "[scene]") {
    World a;
    World b;
    build_scene(a);
    build_scene(b);

    std::vector<u8> ba;
    std::vector<u8> bb;
    psynder::scene::serialize_scene(a, ba);
    psynder::scene::serialize_scene(b, bb);

    REQUIRE(ba.size() == bb.size());
    CHECK(ba == bb);

    // A second serialization of the same World is stable too.
    std::vector<u8> ba2;
    psynder::scene::serialize_scene(a, ba2);
    CHECK(ba == ba2);
}
