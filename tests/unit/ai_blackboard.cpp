// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_blackboard.cpp — a per-agent key->value scratch store for
// behavior trees: each typed value round-trips, an unset or wrong-type get
// fails and leaves out untouched (the type-tag rule), overwriting a key reuses
// its slot, many keys are independent, has/remove/clear/size behave, and the
// same op sequence reproduces the same gets bit-for-bit.

#include "ai/Blackboard.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::ai;

TEST_CASE("ai: blackboard round-trips each value type", "[ai][blackboard]") {
    Blackboard bb;

    bb.set_float(1u, 3.5f);
    bb.set_int(2u, -7);
    bb.set_vec3(3u, {1.0f, 2.0f, 3.0f});
    bb.set_bool(4u, true);

    f32 f = 0.0f;
    REQUIRE(bb.get_float(1u, f));
    REQUIRE(f == Catch::Approx(3.5f));

    i32 i = 0;
    REQUIRE(bb.get_int(2u, i));
    REQUIRE(i == -7);

    math::Vec3 v{};
    REQUIRE(bb.get_vec3(3u, v));
    REQUIRE(v.x == Catch::Approx(1.0f));
    REQUIRE(v.y == Catch::Approx(2.0f));
    REQUIRE(v.z == Catch::Approx(3.0f));

    bool b = false;
    REQUIRE(bb.get_bool(4u, b));
    REQUIRE(b == true);

    REQUIRE(bb.size() == 4u);
}

TEST_CASE("ai: blackboard get on an unset key fails and leaves out untouched",
          "[ai][blackboard]") {
    Blackboard bb;

    f32 f = 42.0f;
    REQUIRE_FALSE(bb.get_float(99u, f));
    REQUIRE(f == Catch::Approx(42.0f));  // sentinel preserved

    i32 i = 1234;
    REQUIRE_FALSE(bb.get_int(99u, i));
    REQUIRE(i == 1234);

    math::Vec3 v{5.0f, 6.0f, 7.0f};
    REQUIRE_FALSE(bb.get_vec3(99u, v));
    REQUIRE(v.x == Catch::Approx(5.0f));
    REQUIRE(v.z == Catch::Approx(7.0f));

    bool b = true;
    REQUIRE_FALSE(bb.get_bool(99u, b));
    REQUIRE(b == true);

    REQUIRE(bb.size() == 0u);
}

TEST_CASE("ai: blackboard overwriting a key updates the value and keeps size",
          "[ai][blackboard]") {
    Blackboard bb;
    bb.set_float(8u, 1.0f);
    REQUIRE(bb.size() == 1u);

    bb.set_float(8u, 2.5f);  // same key, same type: reuse slot
    REQUIRE(bb.size() == 1u);

    f32 f = 0.0f;
    REQUIRE(bb.get_float(8u, f));
    REQUIRE(f == Catch::Approx(2.5f));
}

TEST_CASE("ai: blackboard reading a key as the wrong type fails the type-tag rule",
          "[ai][blackboard]") {
    Blackboard bb;
    bb.set_float(5u, 9.0f);

    // Stored as a float; every other-typed getter must reject it (no bit reinterp).
    i32 i = 111;
    REQUIRE_FALSE(bb.get_int(5u, i));
    REQUIRE(i == 111);

    math::Vec3 v{8.0f, 8.0f, 8.0f};
    REQUIRE_FALSE(bb.get_vec3(5u, v));
    REQUIRE(v.x == Catch::Approx(8.0f));

    bool b = false;
    REQUIRE_FALSE(bb.get_bool(5u, b));
    REQUIRE(b == false);

    // The float getter still works for the correct type.
    f32 f = 0.0f;
    REQUIRE(bb.get_float(5u, f));
    REQUIRE(f == Catch::Approx(9.0f));
}

TEST_CASE("ai: blackboard retagging a key to a new type changes which getter wins",
          "[ai][blackboard]") {
    Blackboard bb;
    bb.set_float(3u, 1.5f);

    f32 f = 0.0f;
    REQUIRE(bb.get_float(3u, f));
    REQUIRE(f == Catch::Approx(1.5f));

    // Re-set the same key as an int: it now holds an int, and the float getter
    // must fail. size stays 1 (slot reused, retagged).
    bb.set_int(3u, 42);
    REQUIRE(bb.size() == 1u);

    f = 7.0f;
    REQUIRE_FALSE(bb.get_float(3u, f));
    REQUIRE(f == Catch::Approx(7.0f));  // untouched on the failed get

    i32 i = 0;
    REQUIRE(bb.get_int(3u, i));
    REQUIRE(i == 42);
}

TEST_CASE("ai: blackboard has, remove, clear, and size behave", "[ai][blackboard]") {
    Blackboard bb;
    REQUIRE(bb.size() == 0u);
    REQUIRE_FALSE(bb.has(1u));

    bb.set_float(1u, 1.0f);
    bb.set_int(2u, 2);
    bb.set_bool(3u, true);
    REQUIRE(bb.size() == 3u);
    REQUIRE(bb.has(1u));
    REQUIRE(bb.has(2u));
    REQUIRE(bb.has(3u));
    REQUIRE_FALSE(bb.has(4u));

    // remove a present key: gone, size drops, others survive.
    bb.remove(2u);
    REQUIRE_FALSE(bb.has(2u));
    REQUIRE(bb.size() == 2u);
    i32 i = 99;
    REQUIRE_FALSE(bb.get_int(2u, i));
    REQUIRE(i == 99);
    REQUIRE(bb.has(1u));
    REQUIRE(bb.has(3u));

    // remove a missing key: a no-op.
    bb.remove(2u);
    REQUIRE(bb.size() == 2u);

    bb.clear();
    REQUIRE(bb.size() == 0u);
    REQUIRE_FALSE(bb.has(1u));
    REQUIRE_FALSE(bb.has(3u));
}

TEST_CASE("ai: blackboard stores many keys independently", "[ai][blackboard]") {
    Blackboard bb;

    // Fill several keys of mixed types; each must read back its own value.
    for (u32 k = 0; k < 10u; ++k) {
        bb.set_float(k, static_cast<f32>(k) + 0.25f);
    }
    REQUIRE(bb.size() == 10u);

    for (u32 k = 0; k < 10u; ++k) {
        f32 f = -1.0f;
        REQUIRE(bb.get_float(k, f));
        REQUIRE(f == Catch::Approx(static_cast<f32>(k) + 0.25f));
    }

    // Overwriting one key in the middle must not disturb its neighbours.
    bb.set_vec3(5u, {100.0f, 0.0f, 0.0f});  // retag key 5 to a vec3
    f32 f4 = 0.0f;
    f32 f6 = 0.0f;
    REQUIRE(bb.get_float(4u, f4));
    REQUIRE(f4 == Catch::Approx(4.25f));
    REQUIRE(bb.get_float(6u, f6));
    REQUIRE(f6 == Catch::Approx(6.25f));
    REQUIRE_FALSE(bb.get_float(5u, f4));  // 5 is now a vec3
    math::Vec3 v{};
    REQUIRE(bb.get_vec3(5u, v));
    REQUIRE(v.x == Catch::Approx(100.0f));
    REQUIRE(bb.size() == 10u);
}

TEST_CASE("ai: blackboard is deterministic for the same op sequence",
          "[ai][blackboard][determinism]") {
    const auto run = []() {
        Blackboard bb;
        std::vector<f32> trace;
        const auto snapshot = [&]() {
            trace.push_back(static_cast<f32>(bb.size()));
            for (u32 k = 0; k < 8u; ++k) {
                f32 f = -1.0f;
                trace.push_back(bb.get_float(k, f) ? f : -1.0f);
                i32 i = -1;
                trace.push_back(bb.get_int(k, i) ? static_cast<f32>(i) : -1.0f);
                math::Vec3 v{-1.0f, -1.0f, -1.0f};
                const bool gv = bb.get_vec3(k, v);
                trace.push_back(gv ? v.x : -1.0f);
                trace.push_back(gv ? v.z : -1.0f);
                bool b = false;
                trace.push_back((bb.get_bool(k, b) && b) ? 1.0f : 0.0f);
                trace.push_back(bb.has(k) ? 1.0f : 0.0f);
            }
        };

        bb.set_float(0u, 1.25f);
        bb.set_int(1u, 7);
        bb.set_vec3(2u, {3.0f, 0.0f, 4.0f});
        bb.set_bool(3u, true);
        snapshot();
        bb.set_float(0u, 9.5f);   // overwrite same type
        bb.set_int(2u, -3);       // retag a vec3 key to int
        bb.remove(1u);            // drop a key
        bb.set_bool(5u, false);   // a new bool key
        snapshot();
        bb.clear();
        bb.set_vec3(4u, {1.0f, 2.0f, 3.0f});
        snapshot();
        return trace;
    };
    REQUIRE(run() == run());
}

TEST_CASE("ai: blackboard drops new keys past its fixed capacity", "[ai][blackboard]") {
    Blackboard bb;
    const u32 cap = static_cast<u32>(Blackboard::capacity());

    // Fill every slot.
    for (u32 k = 0; k < cap; ++k) bb.set_int(k, static_cast<i32>(k));
    REQUIRE(bb.size() == Blackboard::capacity());

    // A brand-new key past capacity is dropped: size unchanged, key absent.
    bb.set_int(cap, 999);
    REQUIRE(bb.size() == Blackboard::capacity());
    REQUIRE_FALSE(bb.has(cap));
    i32 i = -1;
    REQUIRE_FALSE(bb.get_int(cap, i));

    // But re-setting an ALREADY-present key while full still works (slot reuse).
    bb.set_int(0u, 12345);
    REQUIRE(bb.get_int(0u, i));
    REQUIRE(i == 12345);
    REQUIRE(bb.size() == Blackboard::capacity());
}
