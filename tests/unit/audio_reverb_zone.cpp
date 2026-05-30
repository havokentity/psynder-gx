// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: acoustic-space reverb zone math (ReverbZone.h),
// the size-to-reverb-parameters model + zone-boundary crossfade.
//
// Verifies the load-bearing properties of engine/audio/ReverbZone.h:
//   (a) a bigger room yields more wet AND a longer decay than a small room;
//   (b) wet stays in [0,1] and decay >= 0 even for enormous volumes (the
//       saturating map never overshoots), and never exceeds the maxima;
//   (c) a zero / near-zero room is nearly dry with a near-zero tail;
//   (d) reverb_blend interpolates linearly and clamps t at both ends;
//   (e) reverb_lerp blends both fields, with the midpoint being the average;
//   (f) reverb_dry_gain == 1 - wet, clamped to [0,1]; and
//   (g) determinism: identical inputs give bit-identical outputs.

#include "audio/ReverbZone.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::audio;

TEST_CASE("audio: reverb_from_size makes a bigger room wetter and longer", "[audio][reverb]") {
    const f32 max_wet = 0.9f, max_decay = 4.0f;

    const ReverbZoneParams closet    = reverb_from_size(5.0f,     max_wet, max_decay);
    const ReverbZoneParams hall      = reverb_from_size(1000.0f,  max_wet, max_decay);
    const ReverbZoneParams cathedral = reverb_from_size(50000.0f, max_wet, max_decay);

    INFO("closet wet=" << closet.wet << " decay=" << closet.decay_s);
    INFO("hall wet=" << hall.wet << " decay=" << hall.decay_s);
    INFO("cathedral wet=" << cathedral.wet << " decay=" << cathedral.decay_s);

    // Monotonic: more volume => more wet and longer decay.
    REQUIRE(hall.wet      > closet.wet);
    REQUIRE(cathedral.wet > hall.wet);
    REQUIRE(hall.decay_s      > closet.decay_s);
    REQUIRE(cathedral.decay_s > hall.decay_s);

    // At exactly the reference volume the size parameter is 0.5 => half maxima.
    REQUIRE(hall.wet     == Catch::Approx(max_wet   * 0.5f).margin(1e-5f));
    REQUIRE(hall.decay_s == Catch::Approx(max_decay * 0.5f).margin(1e-5f));
}

TEST_CASE("audio: reverb_from_size saturates and stays in range for huge rooms", "[audio][reverb]") {
    const f32 max_wet = 1.0f, max_decay = 6.0f;

    // Sweep a wide range of volumes, including absurd ones; outputs must stay
    // inside the saturating bounds.
    const f32 volumes[] = {0.0f, 1.0f, 100.0f, 1000.0f, 1.0e6f, 1.0e9f, 1.0e12f};
    for (const f32 v : volumes) {
        const ReverbZoneParams p = reverb_from_size(v, max_wet, max_decay);
        INFO("volume=" << v << " wet=" << p.wet << " decay=" << p.decay_s);
        REQUIRE(p.wet >= 0.0f);
        REQUIRE(p.wet <= 1.0f);
        REQUIRE(p.wet <= max_wet);
        REQUIRE(p.decay_s >= 0.0f);
        REQUIRE(p.decay_s <= max_decay);
    }

    // A negative volume is treated as empty (fully dry, no tail).
    const ReverbZoneParams neg = reverb_from_size(-500.0f, max_wet, max_decay);
    REQUIRE(neg.wet     == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(neg.decay_s == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("audio: reverb_from_size makes a near-zero room nearly dry", "[audio][reverb]") {
    const f32 max_wet = 0.8f, max_decay = 3.0f;

    // A zero-volume space is exactly dry with no tail.
    const ReverbZoneParams empty = reverb_from_size(0.0f, max_wet, max_decay);
    REQUIRE(empty.wet     == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(empty.decay_s == Catch::Approx(0.0f).margin(1e-6f));

    // A tiny space (1 m3 against a 1000 m3 reference) is nearly dry / short.
    const ReverbZoneParams tiny = reverb_from_size(1.0f, max_wet, max_decay);
    INFO("tiny wet=" << tiny.wet << " decay=" << tiny.decay_s);
    REQUIRE(tiny.wet     < 0.01f);
    REQUIRE(tiny.decay_s < 0.01f);
    REQUIRE(tiny.wet     > 0.0f);  // not *exactly* zero — there is a room
}

TEST_CASE("audio: reverb_blend interpolates and clamps t at both ends", "[audio][reverb]") {
    const f32 a = 0.2f, b = 0.8f;

    // Endpoints.
    REQUIRE(reverb_blend(a, b, 0.0f) == Catch::Approx(a).margin(1e-6f));
    REQUIRE(reverb_blend(a, b, 1.0f) == Catch::Approx(b).margin(1e-6f));

    // Midpoint is the average.
    REQUIRE(reverb_blend(a, b, 0.5f) == Catch::Approx(0.5f).margin(1e-6f));

    // A quarter of the way across.
    REQUIRE(reverb_blend(a, b, 0.25f) == Catch::Approx(0.35f).margin(1e-6f));

    // t is clamped: out-of-range values saturate at the endpoints, never
    // extrapolating past a or b.
    REQUIRE(reverb_blend(a, b, -2.0f) == Catch::Approx(a).margin(1e-6f));
    REQUIRE(reverb_blend(a, b,  5.0f) == Catch::Approx(b).margin(1e-6f));
}

TEST_CASE("audio: reverb_lerp blends both fields with the midpoint as average", "[audio][reverb]") {
    const ReverbZoneParams dry{0.1f, 0.5f};
    const ReverbZoneParams wet{0.9f, 4.5f};

    // Endpoints select each zone exactly.
    const ReverbZoneParams at0 = reverb_lerp(dry, wet, 0.0f);
    REQUIRE(at0.wet     == Catch::Approx(dry.wet).margin(1e-6f));
    REQUIRE(at0.decay_s == Catch::Approx(dry.decay_s).margin(1e-6f));

    const ReverbZoneParams at1 = reverb_lerp(dry, wet, 1.0f);
    REQUIRE(at1.wet     == Catch::Approx(wet.wet).margin(1e-6f));
    REQUIRE(at1.decay_s == Catch::Approx(wet.decay_s).margin(1e-6f));

    // The boundary midpoint averages both fields.
    const ReverbZoneParams mid = reverb_lerp(dry, wet, 0.5f);
    REQUIRE(mid.wet     == Catch::Approx(0.5f).margin(1e-6f));  // (0.1+0.9)/2
    REQUIRE(mid.decay_s == Catch::Approx(2.5f).margin(1e-6f));  // (0.5+4.5)/2

    // Clamped t carries through to both fields.
    const ReverbZoneParams over = reverb_lerp(dry, wet, 3.0f);
    REQUIRE(over.wet     == Catch::Approx(wet.wet).margin(1e-6f));
    REQUIRE(over.decay_s == Catch::Approx(wet.decay_s).margin(1e-6f));
}

TEST_CASE("audio: reverb_dry_gain is the clamped complement of wet", "[audio][reverb]") {
    // Mid wet => complementary dry.
    REQUIRE(reverb_dry_gain(ReverbZoneParams{0.25f, 1.0f}) == Catch::Approx(0.75f).margin(1e-6f));

    // Fully wet => no direct signal; fully dry => all direct.
    REQUIRE(reverb_dry_gain(ReverbZoneParams{1.0f, 2.0f}) == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(reverb_dry_gain(ReverbZoneParams{0.0f, 0.0f}) == Catch::Approx(1.0f).margin(1e-6f));

    // Out-of-range wet is clamped so dry stays in [0,1].
    REQUIRE(reverb_dry_gain(ReverbZoneParams{1.5f, 1.0f}) == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(reverb_dry_gain(ReverbZoneParams{-0.5f, 1.0f}) == Catch::Approx(1.0f).margin(1e-6f));
}

TEST_CASE("audio: reverb zone math is deterministic across identical calls", "[audio][reverb]") {
    // Same inputs => bit-identical outputs (same-platform determinism).
    const ReverbZoneParams a = reverb_from_size(2500.0f, 0.85f, 3.5f);
    const ReverbZoneParams b = reverb_from_size(2500.0f, 0.85f, 3.5f);
    REQUIRE(a.wet     == b.wet);
    REQUIRE(a.decay_s == b.decay_s);

    REQUIRE(reverb_blend(0.3f, 0.7f, 0.42f) == reverb_blend(0.3f, 0.7f, 0.42f));

    const ReverbZoneParams l1 = reverb_lerp(a, ReverbZoneParams{0.1f, 0.5f}, 0.33f);
    const ReverbZoneParams l2 = reverb_lerp(a, ReverbZoneParams{0.1f, 0.5f}, 0.33f);
    REQUIRE(l1.wet     == l2.wet);
    REQUIRE(l1.decay_s == l2.decay_s);

    REQUIRE(reverb_dry_gain(a) == reverb_dry_gain(a));
}
