// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/spawn_validation.cpp — deterministic walkable-spawn filtering
// (engine/world/outdoor/SpawnValidation): the lockstep-safe slope gate applied
// to a set of candidate spawn points, plus the ground-clamp of the survivors.
// The match-side validation that complements TerrainSpawn's farthest-from-enemy
// pick on the Battlefield-light outdoor map.

#include "world/outdoor/SpawnValidation.h"
#include "world/outdoor/HeightfieldQuery.h"  // terrain_height (expected clamp Y)
#include "world/outdoor/Terrain.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::world::outdoor;

namespace {
HeightmapDesc make_desc(const std::vector<u16>& heights, u32 sx, u32 sz,
                        f32 spacing, f32 scale) {
    HeightmapDesc d{};
    d.size_x = sx;
    d.size_z = sz;
    d.spacing = spacing;
    d.height_scale = scale;
    d.heights = heights.data();
    return d;
}

// A 16x16 field, flat at 5 m for the low-X half (columns 0..7) and a steep
// 45deg ramp for the high-X half (columns 8..15). heights[x] = x*100 on the
// ramp half -> 1 m rise per 1 m run (height_scale 0.01, spacing 1 m), i.e. an
// updot ~ cos(45deg) ~ 0.707 that fails a strict 0.8 gate. Sampling in the
// flat half gives updot == 1, which clears any reasonable gate.
std::vector<u16> mixed_field() {
    std::vector<u16> hm(16 * 16, 0u);
    for (u32 z = 0; z < 16; ++z) {
        for (u32 x = 0; x < 16; ++x) {
            hm[z * 16 + x] =
                (x < 8) ? static_cast<u16>(500u)            // flat 5 m
                        : static_cast<u16>((x - 7u) * 100u);  // steep ramp
        }
    }
    return hm;
}
}  // namespace

TEST_CASE("spawn-validation: every candidate on flat ground is kept",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // 5 m everywhere (scale 0.01)
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const std::vector<math::Vec3> candidates{
        {1.0f, 99.0f, 1.0f}, {3.0f, 99.0f, 2.0f}, {5.0f, 99.0f, 4.0f}};

    std::vector<usize> kept;
    filter_walkable_spawns(d, candidates, /*min_updot=*/0.707f, kept);

    // Flat ground (updot == 1) clears the 0.707 gate for all three candidates.
    REQUIRE(kept.size() == 3u);
    REQUIRE(kept[0] == 0u);
    REQUIRE(kept[1] == 1u);
    REQUIRE(kept[2] == 2u);
    REQUIRE(any_walkable_spawn(d, candidates, 0.707f));
    REQUIRE(first_walkable_index(d, candidates, 0.707f) == 0u);
}

TEST_CASE("spawn-validation: survivors clamp to terrain_height + foot offset",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // 500 * 0.01 = 5 m everywhere
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const std::vector<math::Vec3> candidates{
        {1.0f, 99.0f, 1.0f}, {3.0f, 99.0f, 2.0f}, {5.0f, 99.0f, 4.0f}};

    std::vector<math::Vec3> snapped;
    clamp_walkable_spawns(d, candidates, /*min_updot=*/0.707f, /*foot=*/0.9f,
                          snapped);

    REQUIRE(snapped.size() == 3u);
    // XZ is preserved; Y becomes 5 m surface + 0.9 m foot offset.
    REQUIRE(snapped[0].x == Catch::Approx(1.0f));
    REQUIRE(snapped[0].z == Catch::Approx(1.0f));
    REQUIRE(snapped[0].y == Catch::Approx(5.9f));
    REQUIRE(snapped[1].x == Catch::Approx(3.0f));
    REQUIRE(snapped[1].z == Catch::Approx(2.0f));
    REQUIRE(snapped[1].y == Catch::Approx(5.9f));
    REQUIRE(snapped[2].x == Catch::Approx(5.0f));
    REQUIRE(snapped[2].z == Catch::Approx(4.0f));
    REQUIRE(snapped[2].y == Catch::Approx(5.9f));
}

TEST_CASE("spawn-validation: steep candidates are dropped, flat ones survive",
          "[terrain][gameplay]") {
    const std::vector<u16> hm = mixed_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // Index 0 + 2 sit in the flat half (x < 8); index 1 + 3 sit on the steep
    // 45deg ramp half (x > 8). A strict 0.8 (~37deg) gate keeps only the flat.
    const std::vector<math::Vec3> candidates{
        {2.0f, 99.0f, 4.0f},   // flat   -> kept
        {12.0f, 99.0f, 4.0f},  // steep  -> dropped
        {5.0f, 99.0f, 8.0f},   // flat   -> kept
        {13.0f, 99.0f, 8.0f}}; // steep  -> dropped

    std::vector<usize> kept;
    filter_walkable_spawns(d, candidates, /*min_updot=*/0.8f, kept);

    // Only the flat candidates remain, in ascending index order (0, 2).
    REQUIRE(kept.size() == 2u);
    REQUIRE(kept[0] == 0u);
    REQUIRE(kept[1] == 2u);

    // clamp_walkable_spawns drops the same steep candidates; the survivors snap
    // to the flat 5 m surface (no foot offset here).
    std::vector<math::Vec3> snapped;
    clamp_walkable_spawns(d, candidates, /*min_updot=*/0.8f, /*foot=*/0.0f,
                          snapped);
    REQUIRE(snapped.size() == 2u);
    REQUIRE(snapped[0].x == Catch::Approx(2.0f));
    REQUIRE(snapped[0].z == Catch::Approx(4.0f));
    REQUIRE(snapped[0].y == Catch::Approx(terrain_height(d, 2.0f, 4.0f)));
    REQUIRE(snapped[0].y == Catch::Approx(5.0f));
    REQUIRE(snapped[1].x == Catch::Approx(5.0f));
    REQUIRE(snapped[1].z == Catch::Approx(8.0f));
    REQUIRE(snapped[1].y == Catch::Approx(5.0f));
}

TEST_CASE("spawn-validation: first_walkable_index skips leading steep candidates",
          "[terrain][gameplay]") {
    const std::vector<u16> hm = mixed_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // Index 0 is steep, index 1 is flat: the lowest WALKABLE index is 1.
    const std::vector<math::Vec3> candidates{
        {12.0f, 99.0f, 4.0f},  // steep
        {3.0f, 99.0f, 4.0f}};  // flat

    REQUIRE(first_walkable_index(d, candidates, 0.8f) == 1u);
}

TEST_CASE("spawn-validation: all-steep candidates leave nothing walkable",
          "[terrain][gameplay]") {
    const std::vector<u16> hm = mixed_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // Both candidates sit on the 45deg ramp half; a 0.8 gate rejects both.
    const std::vector<math::Vec3> candidates{
        {12.0f, 99.0f, 4.0f}, {13.0f, 99.0f, 9.0f}};

    std::vector<usize> kept;
    filter_walkable_spawns(d, candidates, /*min_updot=*/0.8f, kept);
    REQUIRE(kept.empty());

    std::vector<math::Vec3> snapped;
    clamp_walkable_spawns(d, candidates, /*min_updot=*/0.8f, /*foot=*/0.0f,
                          snapped);
    REQUIRE(snapped.empty());

    REQUIRE_FALSE(any_walkable_spawn(d, candidates, 0.8f));
    // No survivor -> first_walkable_index returns the size sentinel.
    REQUIRE(first_walkable_index(d, candidates, 0.8f) == candidates.size());
}

TEST_CASE("spawn-validation: any_walkable_spawn is true on a mixed set",
          "[terrain][gameplay]") {
    const std::vector<u16> hm = mixed_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    const std::vector<math::Vec3> candidates{
        {12.0f, 99.0f, 4.0f},  // steep
        {3.0f, 99.0f, 4.0f}};  // flat -> at least one walkable

    REQUIRE(any_walkable_spawn(d, candidates, 0.8f));
}

TEST_CASE("spawn-validation: empty candidates yield empty outputs",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);
    const std::span<const math::Vec3> none{};

    std::vector<usize> kept{42u, 7u};  // pre-seeded -> must be cleared
    filter_walkable_spawns(d, none, 0.707f, kept);
    REQUIRE(kept.empty());

    std::vector<math::Vec3> snapped{{1.0f, 2.0f, 3.0f}};  // pre-seeded
    clamp_walkable_spawns(d, none, 0.707f, 0.0f, snapped);
    REQUIRE(snapped.empty());

    REQUIRE_FALSE(any_walkable_spawn(d, none, 0.707f));
    REQUIRE(first_walkable_index(d, none, 0.707f) == 0u);  // size() == 0
}

TEST_CASE("spawn-validation: identical runs produce identical out_indices",
          "[terrain][gameplay][determinism]") {
    const std::vector<u16> hm = mixed_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    const std::vector<math::Vec3> candidates{
        {2.0f, 99.0f, 4.0f},  {12.0f, 99.0f, 4.0f},
        {5.0f, 99.0f, 8.0f},  {13.0f, 99.0f, 8.0f}};

    std::vector<usize> a;
    std::vector<usize> b;
    filter_walkable_spawns(d, candidates, 0.8f, a);
    filter_walkable_spawns(d, candidates, 0.8f, b);

    REQUIRE(a == b);  // pure algebra, no transcendental: bit-identical
}
