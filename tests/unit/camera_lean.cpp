// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_lean.cpp — peek / lean view modifier (lean around cover).

#include "camera/Lean.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace psynder;
using namespace psynder::camera;

namespace {
LeanParams make_params() {
    LeanParams p;
    p.max_lateral_m = 0.4f;
    p.max_roll_deg = 8.0f;
    p.lerp_rate_per_s = 12.0f;
    return p;
}
}  // namespace

TEST_CASE("lean: input maps left/right/both/neither", "[camera]") {
    CHECK(lean_input(false, false) == Catch::Approx(0.0f));   // neither => centred
    CHECK(lean_input(true, false) == Catch::Approx(-1.0f));   // left only => -1
    CHECK(lean_input(false, true) == Catch::Approx(1.0f));    // right only => +1
    CHECK(lean_input(true, true) == Catch::Approx(0.0f));     // both => cancel
}

TEST_CASE("lean: update eases toward the target and converges", "[camera]") {
    const LeanParams p = make_params();
    LeanState s;  // starts centred at 0
    // Hold right (target +1). Each step moves part-way, never past it.
    f32 prev = s.amount;
    for (int i = 0; i < 300; ++i) {
        lean_update(s, p, /*target*/ 1.0f, 1.0f / 60.0f);
        CHECK(s.amount >= prev - 1e-4f);   // monotonic toward +1
        CHECK(s.amount <= 1.0f + 1e-4f);   // never overshoots
        prev = s.amount;
    }
    CHECK(s.amount == Catch::Approx(1.0f).margin(0.001f));  // converged to full right
}

TEST_CASE("lean: a single big step lands exactly on the target", "[camera]") {
    LeanParams p = make_params();
    p.lerp_rate_per_s = 1000.0f;  // huge rate
    LeanState s;
    // rate*dt = 1000 -> clamped to 1, so one step lands exactly on +1, not past.
    lean_update(s, p, 1.0f, 1.0f);
    CHECK(s.amount == Catch::Approx(1.0f));
}

TEST_CASE("lean: releasing eases back to centre", "[camera]") {
    const LeanParams p = make_params();
    LeanState s;
    // Lean fully left first.
    for (int i = 0; i < 300; ++i) lean_update(s, p, -1.0f, 1.0f / 60.0f);
    CHECK(s.amount == Catch::Approx(-1.0f).margin(0.001f));
    // Release (target 0): eases back toward centre.
    for (int i = 0; i < 300; ++i) lean_update(s, p, 0.0f, 1.0f / 60.0f);
    CHECK(s.amount == Catch::Approx(0.0f).margin(0.001f));
}

TEST_CASE("lean: an out-of-range target is clamped before easing", "[camera]") {
    LeanParams p = make_params();
    p.lerp_rate_per_s = 1000.0f;  // snap in one step
    LeanState s;
    lean_update(s, p, 5.0f, 1.0f);   // target clamps to +1
    CHECK(s.amount == Catch::Approx(1.0f));
    lean_update(s, p, -5.0f, 1.0f);  // target clamps to -1
    CHECK(s.amount == Catch::Approx(-1.0f));
}

TEST_CASE("lean: sample scales lateral + roll by amount", "[camera]") {
    const LeanParams p = make_params();

    // Centred: both offsets exactly 0.
    LeanState centre;  // amount == 0
    const LeanOffset c = lean_sample(centre, p);
    CHECK(c.lateral_m == Catch::Approx(0.0f));
    CHECK(c.roll_deg == Catch::Approx(0.0f));

    // Full right: positive lateral + positive roll at the authored peaks.
    LeanState right;
    right.amount = 1.0f;
    const LeanOffset r = lean_sample(right, p);
    CHECK(r.lateral_m == Catch::Approx(0.4f));
    CHECK(r.roll_deg == Catch::Approx(8.0f));

    // Full left mirrors: negative lateral + negative roll.
    LeanState left;
    left.amount = -1.0f;
    const LeanOffset l = lean_sample(left, p);
    CHECK(l.lateral_m == Catch::Approx(-0.4f));
    CHECK(l.roll_deg == Catch::Approx(-8.0f));

    // Half lean is linear: exactly half the peaks.
    LeanState half;
    half.amount = 0.5f;
    const LeanOffset h = lean_sample(half, p);
    CHECK(h.lateral_m == Catch::Approx(0.2f));
    CHECK(h.roll_deg == Catch::Approx(4.0f));
}

TEST_CASE("lean: a non-finite or zero dt leaves the lean untouched", "[camera]") {
    const LeanParams p = make_params();
    LeanState s;
    // Ease part-way right so there's a non-trivial value to protect.
    for (int i = 0; i < 5; ++i) lean_update(s, p, 1.0f, 1.0f / 60.0f);
    const f32 before = s.amount;

    lean_update(s, p, 1.0f, 0.0f);   // dt 0 -> no move
    CHECK(s.amount == before);
    lean_update(s, p, 1.0f, -1.0f);  // negative dt -> no move
    CHECK(s.amount == before);
    const f32 inf = std::numeric_limits<f32>::infinity();
    lean_update(s, p, 1.0f, inf);    // non-finite dt -> no move
    CHECK(s.amount == before);
}

TEST_CASE("lean: a non-finite target leaves the lean untouched", "[camera]") {
    const LeanParams p = make_params();
    LeanState s;
    for (int i = 0; i < 5; ++i) lean_update(s, p, 1.0f, 1.0f / 60.0f);
    const f32 before = s.amount;

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    lean_update(s, p, nan, 1.0f / 60.0f);  // non-finite target -> no move
    CHECK(s.amount == before);
}

TEST_CASE("lean: reset centres the lean", "[camera]") {
    const LeanParams p = make_params();
    LeanState s;
    for (int i = 0; i < 50; ++i) lean_update(s, p, 1.0f, 1.0f / 60.0f);
    CHECK(s.amount != Catch::Approx(0.0f));
    lean_reset(s);
    CHECK(s.amount == Catch::Approx(0.0f));
}

TEST_CASE("lean: update is deterministic", "[camera][determinism]") {
    const LeanParams p = make_params();
    LeanState a;  // both start centred
    LeanState b;
    for (int i = 0; i < 60; ++i) {
        // Vary the held lean over the sequence: left, centre, right, ...
        const bool left = (i % 3) == 0;
        const bool right = (i % 4) == 0;
        const f32 target = lean_input(left, right);
        lean_update(a, p, target, 1.0f / 90.0f);
        lean_update(b, p, target, 1.0f / 90.0f);
    }
    CHECK(a.amount == b.amount);  // bit-identical after identical sequences
}
