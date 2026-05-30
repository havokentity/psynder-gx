// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_view_bob.cpp
//
// Lane 16 — deterministic walk-cycle head-bob. Validates the contract
// documented in engine/camera/ViewBob.h:
//   - bob_advance grows the phase with DISTANCE travelled; a stopped pawn (zero
//     distance) does NOT advance the phase (the bob freezes).
//   - Advancing by exactly cycle_len_m returns the phase to its start, so the
//     sampled offset matches the pre-advance offset (one full cycle is a loop).
//   - The vertical bounce runs at DOUBLE the lateral frequency (the figure-8):
//     vertical hits its amplitude extreme at phase pi/4 while lateral is still
//     mid-rise, proving sin(2*phase) vs sin(phase).
//   - Every offset channel is bounded by its amplitude (|sin| <= 1).
//   - bob_reset zeroes the phase.
//   - cycle_len_m <= 0 (and a non-finite distance) is guarded: the phase is
//     left unchanged with no NaN/Inf.
//   - Two identical advance+sample sequences are bit-identical (== on the raw
//     f32 fields — the same-platform replay-safety guarantee).

#include "camera/ViewBob.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

using namespace psynder;
using namespace psynder::camera;

namespace {

// A representative walk-bob tuning shared by most cases.
BobParams make_params() noexcept {
    BobParams p{};
    p.vertical_amp_m = 0.06f;   // 6 cm head bounce
    p.lateral_amp_m  = 0.03f;   // 3 cm body sway
    p.roll_amp_deg   = 1.5f;    // gentle view tip
    p.cycle_len_m    = 1.8f;    // one stride pair per 1.8 m travelled
    return p;
}

// Largest absolute positional component of an offset.
f32 max_abs_pos(const BobOffset& o) noexcept {
    f32 m = std::fabs(o.pos[0]);
    m = std::fmax(m, std::fabs(o.pos[1]));
    m = std::fmax(m, std::fabs(o.pos[2]));
    return m;
}

}  // namespace

TEST_CASE("view_bob: state and params are trivially copyable PODs", "[camera]") {
    REQUIRE(std::is_trivially_copyable_v<BobState>);
    REQUIRE(std::is_trivially_copyable_v<BobParams>);
    REQUIRE(std::is_trivially_copyable_v<BobOffset>);
    REQUIRE(sizeof(BobState) == 4);
}

TEST_CASE("view_bob: a stopped pawn does not advance the phase", "[camera]") {
    const BobParams p = make_params();

    // Walk a bit so the phase is somewhere non-trivial.
    BobState s{};
    bob_advance(s, 0.5f, p);
    const f32 moving_phase = s.phase;
    REQUIRE(moving_phase > 0.0f);

    // Zero distance: the bob freezes — phase is untouched.
    bob_advance(s, 0.0f, p);
    REQUIRE(s.phase == moving_phase);

    // A negative distance (no forward travel) is likewise a no-op.
    bob_advance(s, -1.0f, p);
    REQUIRE(s.phase == moving_phase);

    // ...and the sampled offset is therefore frozen too.
    const BobOffset before = bob_sample(s, p);
    bob_advance(s, 0.0f, p);
    const BobOffset after = bob_sample(s, p);
    REQUIRE(after.pos[0]   == before.pos[0]);
    REQUIRE(after.pos[1]   == before.pos[1]);
    REQUIRE(after.roll_deg == before.roll_deg);
}

TEST_CASE("view_bob: one full cycle_len_m returns the phase to its start",
          "[camera]") {
    const BobParams p = make_params();

    BobState s{};
    // Advance to some starting phase first (a partial stride).
    bob_advance(s, 0.4f, p);
    const f32 start_phase = s.phase;
    const BobOffset start_offset = bob_sample(s, p);

    // Now travel exactly one full cycle. The phase wraps back to where it was.
    bob_advance(s, p.cycle_len_m, p);
    REQUIRE(s.phase == Catch::Approx(start_phase).margin(1e-5f));

    // And the offset matches the pre-advance offset (the bob is periodic).
    const BobOffset end_offset = bob_sample(s, p);
    REQUIRE(end_offset.pos[0]   == Catch::Approx(start_offset.pos[0]).margin(1e-5f));
    REQUIRE(end_offset.pos[1]   == Catch::Approx(start_offset.pos[1]).margin(1e-5f));
    REQUIRE(end_offset.roll_deg == Catch::Approx(start_offset.roll_deg).margin(1e-5f));

    // Splitting the same distance into many small steps lands at the same phase
    // (distance accumulates linearly into the phase).
    BobState chunked{};
    bob_advance(chunked, 0.4f, p);
    const int kSteps = 10;
    for (int i = 0; i < kSteps; ++i) {
        bob_advance(chunked, p.cycle_len_m / static_cast<f32>(kSteps), p);
    }
    REQUIRE(chunked.phase == Catch::Approx(start_phase).margin(1e-4f));
}

TEST_CASE("view_bob: vertical bounces at double the lateral frequency "
          "(figure-8)",
          "[camera]") {
    const BobParams p = make_params();

    // Drive the phase to exactly pi/4 by travelling cycle_len_m / 8 from rest
    // (phase = 2*pi * dist / cycle_len = 2*pi * (1/8) = pi/4).
    BobState s{};
    bob_advance(s, p.cycle_len_m / 8.0f, p);
    REQUIRE(s.phase == Catch::Approx(3.14159265f / 4.0f).margin(1e-5f));

    const BobOffset o = bob_sample(s, p);

    // At phase = pi/4: sin(2*phase) = sin(pi/2) = 1, so the VERTICAL channel is
    // at its positive amplitude extreme...
    REQUIRE(o.pos[1] == Catch::Approx(p.vertical_amp_m).margin(1e-5f));

    // ...while the LATERAL channel is only sin(pi/4) ~= 0.7071 of its amplitude,
    // i.e. NOT yet at its own extreme. This is the double-frequency relationship.
    REQUIRE(o.pos[0] ==
            Catch::Approx(p.lateral_amp_m * 0.70710678f).margin(1e-5f));

    // Roll shares the lateral (single) frequency.
    REQUIRE(o.roll_deg ==
            Catch::Approx(p.roll_amp_deg * 0.70710678f).margin(1e-5f));

    // Cross-check the explicit waveform identity at this phase:
    // vertical == amp * sin(2 * phase); lateral == amp * sin(phase).
    REQUIRE(o.pos[1] ==
            Catch::Approx(p.vertical_amp_m * std::sin(2.0f * s.phase)).margin(1e-6f));
    REQUIRE(o.pos[0] ==
            Catch::Approx(p.lateral_amp_m * std::sin(s.phase)).margin(1e-6f));

    // Over a HALF cycle the lateral sway completes a half-turn (sin pi == 0)
    // while the vertical has completed a FULL bounce (sin 2pi == 0) — two
    // vertical zero-crossings of the same kind per one lateral cycle.
    BobState half{};
    bob_advance(half, p.cycle_len_m / 2.0f, p);   // phase = pi
    const BobOffset oh = bob_sample(half, p);
    REQUIRE(oh.pos[0] == Catch::Approx(0.0f).margin(1e-5f));   // sin(pi)
    REQUIRE(oh.pos[1] == Catch::Approx(0.0f).margin(1e-5f));   // sin(2pi)
}

TEST_CASE("view_bob: every channel is bounded by its amplitude", "[camera]") {
    const BobParams p = make_params();
    const f32 eps = 1e-5f;

    // Walk many small steps across more than a full cycle and check the bound
    // at every sample — |sin| <= 1, so each channel never exceeds its amplitude.
    BobState s{};
    const f32 max_pos_amp = std::fmax(p.vertical_amp_m, p.lateral_amp_m);
    for (int i = 0; i < 400; ++i) {
        bob_advance(s, 0.02f, p);
        const BobOffset o = bob_sample(s, p);
        REQUIRE(std::fabs(o.pos[0])   <= p.lateral_amp_m  + eps);
        REQUIRE(std::fabs(o.pos[1])   <= p.vertical_amp_m + eps);
        REQUIRE(o.pos[2] == 0.0f);                       // no fore/aft bob
        REQUIRE(std::fabs(o.roll_deg) <= p.roll_amp_deg  + eps);
        REQUIRE(max_abs_pos(o) <= max_pos_amp + eps);

        // The phase stays wrapped into [0, 2*pi) for numeric stability.
        REQUIRE(s.phase >= 0.0f);
        REQUIRE(s.phase < 6.2831854f);
    }
}

TEST_CASE("view_bob: reset zeroes the phase", "[camera]") {
    const BobParams p = make_params();
    BobState s{};
    bob_advance(s, 0.9f, p);
    REQUIRE(s.phase > 0.0f);

    bob_reset(s);
    REQUIRE(s.phase == 0.0f);

    // At zero phase every channel is exactly zero (sin 0 == 0) — neutral rest.
    const BobOffset o = bob_sample(s, p);
    REQUIRE(o.pos[0]   == 0.0f);
    REQUIRE(o.pos[1]   == 0.0f);
    REQUIRE(o.pos[2]   == 0.0f);
    REQUIRE(o.roll_deg == 0.0f);
}

TEST_CASE("view_bob: a non-positive cycle length is guarded (no NaN/Inf)",
          "[camera]") {
    BobParams p = make_params();
    BobState s{};
    bob_advance(s, 0.5f, p);          // establish a valid phase first
    const f32 good_phase = s.phase;

    // cycle_len_m == 0 would divide by zero — the guard leaves the phase as-is.
    p.cycle_len_m = 0.0f;
    bob_advance(s, 1.0f, p);
    REQUIRE(s.phase == good_phase);
    REQUIRE(std::isfinite(s.phase));

    // A negative cycle length is likewise rejected.
    p.cycle_len_m = -2.0f;
    bob_advance(s, 1.0f, p);
    REQUIRE(s.phase == good_phase);
    REQUIRE(std::isfinite(s.phase));

    // A non-finite distance is rejected too (no NaN/Inf leaks into the state).
    p.cycle_len_m = make_params().cycle_len_m;
    bob_advance(s, std::numeric_limits<f32>::infinity(), p);
    REQUIRE(s.phase == good_phase);
    REQUIRE(std::isfinite(s.phase));
    bob_advance(s, std::nanf(""), p);
    REQUIRE(s.phase == good_phase);
    REQUIRE(std::isfinite(s.phase));
}

TEST_CASE("view_bob: identical advance+sample sequences are bit-identical "
          "(determinism)",
          "[camera]") {
    const BobParams p = make_params();

    auto run = [&](BobState& s) -> BobOffset {
        // A varied walk: different per-tick distances, including a couple of
        // stops, to exercise the advance + wrap path thoroughly.
        const f32 steps[] = {0.10f, 0.25f, 0.0f, 0.40f, 0.05f, 0.0f,
                             0.33f, 0.60f, 0.12f, 0.48f};
        for (f32 d : steps) bob_advance(s, d, p);
        return bob_sample(s, p);
    };

    BobState a{};
    BobState b{};
    const BobOffset oa = run(a);
    const BobOffset ob = run(b);

    // Bit-identical phase and every offset field (== on the raw f32s, not Approx).
    REQUIRE(a.phase == b.phase);
    REQUIRE(oa.pos[0]   == ob.pos[0]);
    REQUIRE(oa.pos[1]   == ob.pos[1]);
    REQUIRE(oa.pos[2]   == ob.pos[2]);
    REQUIRE(oa.roll_deg == ob.roll_deg);
}
