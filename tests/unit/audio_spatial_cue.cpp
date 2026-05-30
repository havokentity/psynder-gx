// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: listener-relative spatial cues (SpatialCue.h),
// the world-geometry-to-scalars front-end that feeds PositionalMix.h.
//
// Verifies the load-bearing properties of engine/audio/SpatialCue.h:
//   (a) distance is the correct Euclidean length for a known offset;
//   (b) distance_attenuation is 1.0 within the reference radius, decreases
//       monotonically through the [ref,max] band, and is 0 at/beyond max;
//   (c) azimuth: dead-ahead ~0 (centre pan, L==R), right => positive (pan_lr
//       gives R>L), left => negative (L>R), and a source directly behind folds
//       to a hard side-pan WITHOUT flipping its left/right sign;
//   (d) doppler_pitch > 1 closing, < 1 receding, == 1 at zero, clamped at the
//       extremes;
//   (e) a SpatialCue azimuth feeds pan_lr to give constant-power L,R; and
//   (f) determinism: two identical calls are bit-identical.

#include "audio/SpatialCue.h"
#include "audio/PositionalMix.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::audio;

// A listener at the origin facing +z (forward), with +x as the right ear.
// This is the canonical pose the azimuth cases reason about.
static ListenerPose make_listener() noexcept {
    return ListenerPose{
        math::Vec3{0.0f, 0.0f, 0.0f},  // position
        math::Vec3{0.0f, 0.0f, 1.0f},  // forward (+z)
        math::Vec3{1.0f, 0.0f, 0.0f},  // right   (+x)
    };
}

TEST_CASE("audio: source_distance is the euclidean length of the offset", "[audio][spatial]") {
    const ListenerPose l = make_listener();

    // 3-4-5 triangle in the x/y plane => distance 5 m.
    const f32 d = source_distance_m(l, math::Vec3{3.0f, 4.0f, 0.0f});
    REQUIRE(d == Catch::Approx(5.0f).margin(1e-5f));

    // Source coincident with the listener => zero distance.
    REQUIRE(source_distance_m(l, l.position) == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("audio: distance_attenuation is unity within ref, falls off, zero past max", "[audio][spatial]") {
    const f32 ref = 1.0f, max = 100.0f, rolloff = 1.0f;

    // At and under the reference radius: full gain.
    REQUIRE(distance_attenuation(0.0f, ref, max, rolloff) == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(distance_attenuation(ref,  ref, max, rolloff) == Catch::Approx(1.0f).margin(1e-6f));

    // Monotonic decrease across the [ref, max) band.
    const f32 g_near = distance_attenuation(2.0f,  ref, max, rolloff);
    const f32 g_mid  = distance_attenuation(10.0f, ref, max, rolloff);
    const f32 g_far  = distance_attenuation(50.0f, ref, max, rolloff);
    INFO("g_near=" << g_near << " g_mid=" << g_mid << " g_far=" << g_far);
    REQUIRE(g_near < 1.0f);
    REQUIRE(g_mid  < g_near);
    REQUIRE(g_far  < g_mid);
    REQUIRE(g_far  > 0.0f);

    // At and beyond the max radius: silent.
    REQUIRE(distance_attenuation(max,         ref, max, rolloff) == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(distance_attenuation(max + 50.0f, ref, max, rolloff) == Catch::Approx(0.0f).margin(1e-6f));

    // Every sample stays in [0,1].
    for (f32 d = 0.0f; d <= 120.0f; d += 7.0f) {
        const f32 g = distance_attenuation(d, ref, max, rolloff);
        REQUIRE(g >= 0.0f);
        REQUIRE(g <= 1.0f);
    }
}

TEST_CASE("audio: listener_azimuth steers pan_lr the correct way", "[audio][spatial]") {
    const ListenerPose l = make_listener();

    f32 lg = 0.0f, rg = 0.0f;

    // Dead ahead (along +z forward): azimuth ~0, centre pan (L == R).
    const f32 az_front = listener_azimuth(l, math::Vec3{0.0f, 0.0f, 10.0f});
    INFO("az_front=" << az_front);
    REQUIRE(az_front == Catch::Approx(0.0f).margin(1e-5f));
    pan_lr(az_front, lg, rg);
    REQUIRE(lg == Catch::Approx(rg).margin(1e-5f));

    // To the right (+x): positive azimuth => pan_lr gives R > L.
    const f32 az_right = listener_azimuth(l, math::Vec3{10.0f, 0.0f, 0.0f});
    INFO("az_right=" << az_right);
    REQUIRE(az_right > 0.0f);
    pan_lr(az_right, lg, rg);
    REQUIRE(rg > lg);

    // To the left (-x): negative azimuth => pan_lr gives L > R.
    const f32 az_left = listener_azimuth(l, math::Vec3{-10.0f, 0.0f, 0.0f});
    INFO("az_left=" << az_left);
    REQUIRE(az_left < 0.0f);
    pan_lr(az_left, lg, rg);
    REQUIRE(lg > rg);
}

TEST_CASE("audio: a source behind folds to a hard side pan keeping its sign", "[audio][spatial]") {
    const ListenerPose l = make_listener();

    // Directly behind (-z) and slightly to the RIGHT (+x): the raw atan2 angle
    // is in the rear hemisphere and folds to the right edge (+pi/2 = hard
    // right), NOT across to the left. Sign of the right offset is preserved.
    const f32 az_behind_right = listener_azimuth(l, math::Vec3{0.001f, 0.0f, -10.0f});
    INFO("az_behind_right=" << az_behind_right);
    REQUIRE(az_behind_right > 0.0f);
    REQUIRE(az_behind_right == Catch::Approx(math::kHalfPi).margin(1e-4f));

    // Directly behind and slightly to the LEFT (-x): folds to the left edge.
    const f32 az_behind_left = listener_azimuth(l, math::Vec3{-0.001f, 0.0f, -10.0f});
    INFO("az_behind_left=" << az_behind_left);
    REQUIRE(az_behind_left < 0.0f);
    REQUIRE(az_behind_left == Catch::Approx(-math::kHalfPi).margin(1e-4f));

    // The folded value stays inside pan_lr's domain.
    REQUIRE(az_behind_right <= math::kHalfPi);
    REQUIRE(az_behind_left  >= -math::kHalfPi);
}

TEST_CASE("audio: source at the listener position gives centre azimuth", "[audio][spatial]") {
    const ListenerPose l = make_listener();
    const f32 az = listener_azimuth(l, l.position);
    REQUIRE(az == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("audio: doppler_pitch rises closing, falls receding, unity at rest", "[audio][spatial]") {
    // Approaching => pitch up.
    const f32 up = doppler_pitch(30.0f);
    INFO("closing pitch=" << up);
    REQUIRE(up > 1.0f);

    // Receding => pitch down.
    const f32 down = doppler_pitch(-30.0f);
    INFO("receding pitch=" << down);
    REQUIRE(down < 1.0f);

    // Zero closing speed => no shift.
    REQUIRE(doppler_pitch(0.0f) == Catch::Approx(1.0f).margin(1e-6f));

    // Extreme closing (at/over the speed of sound, the singularity) clamps to
    // the max; extreme receding clamps to the min.
    REQUIRE(doppler_pitch(kSpeedOfSoundMps)          == Catch::Approx(kDopplerMaxPitch).margin(1e-6f));
    REQUIRE(doppler_pitch(kSpeedOfSoundMps + 100.0f) == Catch::Approx(kDopplerMaxPitch).margin(1e-6f));
    REQUIRE(doppler_pitch(-100000.0f)                == Catch::Approx(kDopplerMinPitch).margin(1e-6f));

    // Degenerate medium => no shift.
    REQUIRE(doppler_pitch(50.0f, 0.0f) == Catch::Approx(1.0f).margin(1e-6f));
}

TEST_CASE("audio: a spatial azimuth feeds pan_lr as constant power", "[audio][spatial]") {
    const ListenerPose l = make_listener();

    // Sweep a source around the listener; every resulting pan is constant-power.
    const math::Vec3 sources[] = {
        math::Vec3{ 10.0f, 0.0f,  0.0f},   // right
        math::Vec3{-10.0f, 0.0f,  0.0f},   // left
        math::Vec3{  0.0f, 0.0f, 10.0f},   // front
        math::Vec3{  7.0f, 0.0f,  7.0f},   // front-right diagonal
        math::Vec3{ -3.0f, 0.0f, -8.0f},   // behind-left (folded)
    };
    for (const math::Vec3 s : sources) {
        const SpatialResult r = spatialize(l, s, 1.0f, 100.0f, 1.0f);
        f32 lg = 0.0f, rg = 0.0f;
        pan_lr(r.azimuth_rad, lg, rg);
        INFO("source (" << s.x << "," << s.y << "," << s.z
             << ") az=" << r.azimuth_rad << " L=" << lg << " R=" << rg);
        REQUIRE((lg * lg + rg * rg) == Catch::Approx(1.0f).margin(1e-5f));
    }
}

TEST_CASE("audio: spatialize bundles distance attenuation and azimuth", "[audio][spatial]") {
    const ListenerPose l = make_listener();
    const math::Vec3 source{3.0f, 4.0f, 0.0f};  // distance 5 m

    const SpatialResult r = spatialize(l, source, 1.0f, 100.0f, 1.0f);
    REQUIRE(r.distance_m == Catch::Approx(5.0f).margin(1e-5f));
    REQUIRE(r.attenuation == Catch::Approx(distance_attenuation(5.0f, 1.0f, 100.0f, 1.0f)).margin(1e-6f));
    REQUIRE(r.azimuth_rad == Catch::Approx(listener_azimuth(l, source)).margin(1e-6f));
    REQUIRE(r.attenuation > 0.0f);
    REQUIRE(r.attenuation < 1.0f);
}

TEST_CASE("audio: spatial cues are deterministic across identical calls", "[audio][spatial]") {
    const ListenerPose l = make_listener();
    const math::Vec3 source{12.5f, -3.25f, 7.75f};

    // Same inputs => bit-identical outputs (same-platform determinism).
    REQUIRE(source_distance_m(l, source) == source_distance_m(l, source));
    REQUIRE(listener_azimuth(l, source)  == listener_azimuth(l, source));
    REQUIRE(distance_attenuation(20.0f, 1.0f, 100.0f, 1.5f)
            == distance_attenuation(20.0f, 1.0f, 100.0f, 1.5f));
    REQUIRE(doppler_pitch(42.0f) == doppler_pitch(42.0f));

    const SpatialResult a = spatialize(l, source, 1.0f, 100.0f, 1.0f);
    const SpatialResult b = spatialize(l, source, 1.0f, 100.0f, 1.0f);
    REQUIRE(a.distance_m  == b.distance_m);
    REQUIRE(a.attenuation == b.attenuation);
    REQUIRE(a.azimuth_rad == b.azimuth_rad);
}
