// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/physics_core_smoke_lite.cpp
//
// Lane 15 (physics-core) — lightweight world lifecycle smoke.
//
// Motivation: physics_core_smoke.cpp contains the full M0/M1/M2 acceptance
// tests (gravity, applied force, character controller, determinism). Those
// require Jolt Physics to be linked and take non-trivial time.  This file
// provides a tiny subset — world create / destroy — so lane-07 and other
// lanes that iterate quickly can get a fast signal that the Jolt wrapper
// didn't break basic lifecycle even when the heavier tests are being
// debugged.
//
// The file is intentionally minimal: only the create/destroy cycle and the
// WorldDesc defaults test. Add heavier tests here only if they can't live
// in physics_core_smoke.cpp (e.g. a second independent determinism run or
// a bounds-check on WorldDesc values).

#include "physics/core/PublicPhysicsCore.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder::physics;

TEST_CASE("physics-lite: default WorldDesc creates and destroys cleanly",
          "[physics][lite][smoke]") {
    WorldDesc desc{};
    // Verify the defaults are sane before create — the public header
    // documents these values; a future public-header change that silently
    // breaks them would be caught here.
    REQUIRE(desc.gravity_y_m_per_s2 == -9.81f);
    REQUIRE(desc.max_bodies         == 64'000u);
    REQUIRE(desc.max_constraints    == 64'000u);
    REQUIRE(desc.tick_hz            == 120u);

    World* w = create_world(desc);
    REQUIRE(w != nullptr);
    destroy_world(w);
}

TEST_CASE("physics-lite: create_world / destroy_world is re-entrant",
          "[physics][lite][smoke]") {
    // Repeated create+destroy must not leak Jolt state or crash.
    for (int i = 0; i < 3; ++i) {
        World* w = create_world(WorldDesc{});
        REQUIRE(w != nullptr);
        destroy_world(w);
    }
}

TEST_CASE("physics-lite: custom WorldDesc values are accepted",
          "[physics][lite][smoke]") {
    WorldDesc desc{};
    desc.gravity_y_m_per_s2 = -1.62f;    // lunar gravity
    desc.max_bodies         = 1024u;
    desc.max_constraints    = 512u;
    desc.tick_hz            = 60u;

    World* w = create_world(desc);
    REQUIRE(w != nullptr);
    destroy_world(w);
}
