// SPDX-License-Identifier: MIT
// Lane 06 — static smoke test for engine/scene/GxComponents.h.
//
// Validates:
//   1. Every component satisfies trivially-copyable (required by
//      PSYNDER_COMPONENT macro).
//   2. Frozen layout sizes (sizeof assertions already in the header, but
//      duplicate them here so a test failure calls out the regression
//      immediately rather than at compile time deep in a TU).
//   3. Field-offset spot-checks for the ECS-hot components that lane 09
//      Extract.cpp dereferences directly.

#include "scene/GxComponents.h"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using namespace psynder;
using namespace psynder::scene;

// ─── Trivial copyability ─────────────────────────────────────────────────────
static_assert(std::is_trivially_copyable_v<TransformWS>);
static_assert(std::is_trivially_copyable_v<MeshRef>);
static_assert(std::is_trivially_copyable_v<MaterialRef>);
static_assert(std::is_trivially_copyable_v<LightPoint>);
static_assert(std::is_trivially_copyable_v<LightDirectional>);
static_assert(std::is_trivially_copyable_v<CameraComponent>);
static_assert(std::is_trivially_copyable_v<VisibleBit>);

TEST_CASE("GxComponents frozen sizes", "[scene][gxcomponents]") {
    CHECK(sizeof(TransformWS)      == 128);
    CHECK(sizeof(MeshRef)          ==  12);
    CHECK(sizeof(MaterialRef)      ==   8);
    CHECK(sizeof(LightPoint)       ==  32);
    CHECK(sizeof(LightDirectional) ==  32);
    CHECK(sizeof(CameraComponent)  == 160);
    CHECK(sizeof(VisibleBit)       ==   4);
}

TEST_CASE("TransformWS alignment", "[scene][gxcomponents]") {
    CHECK(alignof(TransformWS) >= 16);
    // mtw is the first field; the struct itself must be 16-byte aligned for
    // ECS chunk SIMD loads to work without masking.
    static_assert(offsetof(TransformWS, mtw)      ==  0);
    static_assert(offsetof(TransformWS, prev_mtw) == 64);
}

TEST_CASE("LightPoint physical units sanity", "[scene][gxcomponents]") {
    LightPoint lp{};
    lp.position  = {1.0f, 2.0f, 3.0f};  // metres
    lp.radius    = 10.0f;                // metres
    lp.color     = {1.0f, 0.9f, 0.8f};  // linear sRGB
    lp.intensity = 800.0f;              // lumens (typical 60 W incandescent equiv.)
    CHECK(lp.intensity > 0.0f);
    CHECK(lp.radius    > 0.0f);
}

TEST_CASE("VisibleBit partition enum coverage", "[scene][gxcomponents]") {
    VisibleBit v{};
    v.partition = VisibleBit::Partition::WorldStatic;
    CHECK(static_cast<int>(v.partition) == 0);
    v.partition = VisibleBit::Partition::WorldDynamic;
    CHECK(static_cast<int>(v.partition) == 1);
}

TEST_CASE("Asset handle validity", "[scene][gxcomponents]") {
    // MeshRef default-initialised must be invalid (raw==0).
    MeshRef mr{};
    CHECK(!mr.mesh.valid());

    // MaterialRef same.
    MaterialRef mat{};
    CHECK(!mat.material.valid());
}
