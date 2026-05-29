// SPDX-License-Identifier: MIT
// Locks in the determinism pillar for ECS component identity: the id a
// component *type* receives must be a stable function of the TYPE, NOT of the
// order in which components are first registered/touched. Server-authoritative
// lockstep and snapshot replication key archetype columns by ComponentId, so a
// call-order id (the old, latent-bug scheme: id = ++counter at first touch)
// would differ across runs/builds/translation units and break determinism.
//
// component_id<T>() now derives the id from fnv1a32(component_signature<T>()),
// i.e. an FNV-1a-32 hash of the compiler's per-type signature string. These
// tests assert that property directly and are written so they would FAIL under
// the old sequential-counter scheme.

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

using namespace psynder;
using namespace psynder::scene;

namespace {

// Distinct, test-local component types. Declared in this fixed source order so
// that a call-order id scheme would mint them as a small contiguous run; the
// hash scheme scatters them by type name instead.
struct CidAlpha   { f32 x; };
struct CidBravo   { u32 a; u32 b; };
struct CidCharlie { f64 v; };
struct CidDelta   { u8  flags; };
struct CidEcho    { i32 n[3]; };

}  // namespace

TEST_CASE("scene/component-id: id is the type-signature hash, not a call-order counter",
          "[scene][ecs][determinism]") {
    // The defining property: the id equals the FNV-1a-32 of the type signature.
    // A sequential-counter scheme would instead hand out tiny ids like 1,2,3...
    // which (vanishingly improbably) would not equal these 32-bit hashes.
    REQUIRE(component_id<CidAlpha>()   == fnv1a32(component_signature<CidAlpha>()));
    REQUIRE(component_id<CidBravo>()   == fnv1a32(component_signature<CidBravo>()));
    REQUIRE(component_id<CidCharlie>() == fnv1a32(component_signature<CidCharlie>()));
    REQUIRE(component_id<CidDelta>()   == fnv1a32(component_signature<CidDelta>()));
    REQUIRE(component_id<CidEcho>()    == fnv1a32(component_signature<CidEcho>()));
}

TEST_CASE("scene/component-id: ids are stable regardless of first-touch order",
          "[scene][ecs][determinism]") {
    // Touch the same five types in a *different* order than they were declared
    // and than the previous test queried them. Ids must be unchanged: the id is
    // a pure function of the type, so order of first use cannot move it.
    const ComponentId echo    = component_id<CidEcho>();
    const ComponentId alpha   = component_id<CidAlpha>();
    const ComponentId delta   = component_id<CidDelta>();
    const ComponentId charlie = component_id<CidCharlie>();
    const ComponentId bravo   = component_id<CidBravo>();

    REQUIRE(alpha   == fnv1a32(component_signature<CidAlpha>()));
    REQUIRE(bravo   == fnv1a32(component_signature<CidBravo>()));
    REQUIRE(charlie == fnv1a32(component_signature<CidCharlie>()));
    REQUIRE(delta   == fnv1a32(component_signature<CidDelta>()));
    REQUIRE(echo    == fnv1a32(component_signature<CidEcho>()));
}

TEST_CASE("scene/component-id: relative id ordering tracks the stable key, not declaration order",
          "[scene][ecs][determinism]") {
    // Under a call-order scheme the ids would be monotonic in *declaration /
    // touch* order (alpha<bravo<charlie<delta<echo). Under the hash scheme they
    // are ordered by the type-name hash. Assert the observed id ordering equals
    // the ordering of the independently-computed stable keys, and that it is
    // NOT the declaration order (which proves we are not call-order indexed).
    struct Pair { ComponentId id; ComponentId key; };
    std::array<Pair, 5> ts{{
        { component_id<CidAlpha>(),   fnv1a32(component_signature<CidAlpha>())   },
        { component_id<CidBravo>(),   fnv1a32(component_signature<CidBravo>())   },
        { component_id<CidCharlie>(), fnv1a32(component_signature<CidCharlie>()) },
        { component_id<CidDelta>(),   fnv1a32(component_signature<CidDelta>())   },
        { component_id<CidEcho>(),    fnv1a32(component_signature<CidEcho>())    },
    }};

    // id == key for every type (the mapping is identity over the stable key).
    for (const Pair& p : ts) {
        REQUIRE(p.id == p.key);
    }

    // All five ids are distinct (no accidental collision in this fixture).
    std::array<ComponentId, 5> ids{ ts[0].id, ts[1].id, ts[2].id, ts[3].id, ts[4].id };
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Guard against silently regressing to a call-order counter. A counter
    // scheme yields a dense contiguous run, so (max - min) would equal
    // (count - 1). Hash-derived ids are scattered across the u32 space, so the
    // span vastly exceeds the count. This holds for ANY toolchain (it depends
    // only on the hash being a wide mapping, not on a particular name ordering).
    const ComponentId span = ids.back() - ids.front();
    REQUIRE(span > static_cast<ComponentId>(ids.size()));
}

TEST_CASE("scene/component-id: distinct types backing live archetypes keep distinct columns",
          "[scene][ecs][determinism]") {
    // End-to-end: the hash ids index real archetype columns; adding two
    // different components to one entity must produce two readable, independent
    // columns (i.e. the ids did not collide into one column).
    World w;
    const Entity e = w.create();
    REQUIRE(e.valid());

    w.add<CidAlpha>(e, CidAlpha{ 3.5f });
    w.add<CidBravo>(e, CidBravo{ 7u, 11u });

    CidAlpha* a = w.get<CidAlpha>(e);
    CidBravo* b = w.get<CidBravo>(e);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->x == 3.5f);
    REQUIRE(b->a == 7u);
    REQUIRE(b->b == 11u);
}
