// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/world_wind.cpp — the deterministic COSMETIC wind vector field
// (engine/world/outdoor/Wind): a steady base flow plus a gentle gust that
// oscillates over time and space, sampled by foliage / particle / cloth drift.
// This field is cosmetic and intentionally off the authoritative lockstep tick
// (it uses sin), so the tests assert behaviour + same-platform determinism, not
// cross-platform bit parity.

#include "world/outdoor/Wind.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::world::outdoor;

namespace {
WindParams make_wind(math::Vec3 dir, f32 base, f32 gust, f32 freq, f32 scale) {
    WindParams p{};
    p.base_dir = dir;
    p.base_strength = base;
    p.gust_strength = gust;
    p.gust_frequency_hz = freq;
    p.spatial_scale = scale;
    return p;
}
}  // namespace

TEST_CASE("wind: zero gust returns the steady base flow", "[wind][cosmetic]") {
    // No gust component -> wind is exactly normalize(base_dir) * base_strength.
    const WindParams p =
        make_wind(math::Vec3{2.0f, 0.0f, 0.0f}, 4.0f, 0.0f, 0.5f, 0.1f);

    const math::Vec3 w = wind_at(p, math::Vec3{10.0f, 0.0f, 7.0f}, 3.0f);
    REQUIRE(w.x == Catch::Approx(4.0f).margin(1e-5f));  // +X at base_strength
    REQUIRE(w.y == Catch::Approx(0.0f).margin(1e-5f));  // horizontal
    REQUIRE(w.z == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("wind: a nonzero gust makes the vector vary over time",
          "[wind][cosmetic]") {
    const WindParams p =
        make_wind(math::Vec3{1.0f, 0.0f, 0.0f}, 3.0f, 2.0f, 0.5f, 0.0f);

    const math::Vec3 a = wind_at(p, math::Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    // 0.5 Hz -> a quarter period (peak gust) lands at t = 0.5 s.
    const math::Vec3 b = wind_at(p, math::Vec3{0.0f, 0.0f, 0.0f}, 0.5f);

    // The gust buffets along the perpendicular (Z for an +X base_dir): at t=0
    // sin(0)=0, at t=0.5 sin(pi/2)=1 -> a clear difference in Z.
    REQUIRE(a.z == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(b.z == Catch::Approx(2.0f).margin(1e-4f));
    REQUIRE(std::fabs(b.z - a.z) > 1e-3f);
}

TEST_CASE("wind: the field varies over space", "[wind][cosmetic]") {
    // spatial_scale > 0 -> the gust phase rolls across (x + z), so two distant
    // positions at the same instant generally differ.
    const WindParams p =
        make_wind(math::Vec3{1.0f, 0.0f, 0.0f}, 3.0f, 2.0f, 0.5f, 0.3f);

    const math::Vec3 near = wind_at(p, math::Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
    // Pick a far position whose (x+z) phase shift is ~pi/2 (0.3 * dx = pi/2 ->
    // dx ~ 5.24 m) so the gust sin differs substantially.
    const math::Vec3 far =
        wind_at(p, math::Vec3{5.2359877f, 0.0f, 0.0f}, 1.0f);
    REQUIRE(std::fabs(far.z - near.z) > 1e-3f);

    // With spatial_scale == 0 the field is spatially uniform (time-only).
    const WindParams uni =
        make_wind(math::Vec3{1.0f, 0.0f, 0.0f}, 3.0f, 2.0f, 0.5f, 0.0f);
    const math::Vec3 u0 = wind_at(uni, math::Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
    const math::Vec3 u1 = wind_at(uni, math::Vec3{100.0f, 0.0f, 50.0f}, 1.0f);
    REQUIRE(u0.x == Catch::Approx(u1.x).margin(1e-5f));
    REQUIRE(u0.z == Catch::Approx(u1.z).margin(1e-5f));
}

TEST_CASE("wind: wind_strength_at is the magnitude of wind_at",
          "[wind][cosmetic]") {
    const WindParams p =
        make_wind(math::Vec3{1.0f, 0.0f, 0.0f}, 3.0f, 2.0f, 0.5f, 0.1f);
    const math::Vec3 pos{4.0f, 0.0f, -2.0f};
    const f32 t = 1.25f;

    const math::Vec3 w = wind_at(p, pos, t);
    const f32 expected = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
    REQUIRE(wind_strength_at(p, pos, t) == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("wind: wind_displacement is bounded by sway_amount times the peak",
          "[wind][cosmetic]") {
    const f32 base = 3.0f;
    const f32 gust = 2.0f;
    const WindParams p =
        make_wind(math::Vec3{1.0f, 0.0f, 0.0f}, base, gust, 0.5f, 0.1f);
    const f32 sway = 0.25f;

    // Peak wind speed is bounded by base_strength + gust_strength (the steady
    // and gust components are orthogonal, so the true peak is a touch under the
    // sum); the displacement magnitude must not exceed sway * that peak.
    const f32 bound = sway * (base + gust);

    for (f32 t = 0.0f; t < 4.0f; t += 0.13f) {
        const math::Vec3 d = wind_displacement(p, math::Vec3{1.0f, 0.0f, 2.0f},
                                               t, sway);
        const f32 mag = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        REQUIRE(mag <= bound + 1e-4f);
    }

    // And it really is wind_at scaled by sway.
    const math::Vec3 w = wind_at(p, math::Vec3{1.0f, 0.0f, 2.0f}, 0.7f);
    const math::Vec3 d = wind_displacement(p, math::Vec3{1.0f, 0.0f, 2.0f},
                                           0.7f, sway);
    REQUIRE(d.x == Catch::Approx(w.x * sway).margin(1e-6f));
    REQUIRE(d.z == Catch::Approx(w.z * sway).margin(1e-6f));
}

TEST_CASE("wind: same position and time give the same vector",
          "[wind][cosmetic][determinism]") {
    const WindParams p =
        make_wind(math::Vec3{0.6f, 0.0f, 0.8f}, 3.0f, 2.0f, 0.5f, 0.1f);
    const math::Vec3 pos{12.5f, 0.0f, -3.75f};
    const f32 t = 2.5f;

    const math::Vec3 a = wind_at(p, pos, t);
    const math::Vec3 b = wind_at(p, pos, t);
    REQUIRE(a.x == b.x);  // exact: same platform, same inputs, pure function
    REQUIRE(a.y == b.y);
    REQUIRE(a.z == b.z);
}

TEST_CASE("wind: the default breeze is a gentle horizontal wind",
          "[wind][cosmetic]") {
    // kDefaultWind is a constexpr WindParams (Vec3 is a literal aggregate).
    const math::Vec3 w = wind_at(kDefaultWind, math::Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(w.y == Catch::Approx(0.0f).margin(1e-5f));      // horizontal
    REQUIRE(w.x == Catch::Approx(kDefaultWind.base_strength).margin(1e-5f));
}
