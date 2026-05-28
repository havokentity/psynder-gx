// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/destruction_fracture.cpp
//
// Lane 17 — deterministic precomputed-fracture core (ADR-021, issue #45).
//
// The single load-bearing property of this scaffold is DETERMINISM: fracturing
// with the same seed must yield bit-identical shards so a destruction event
// replays exactly in lockstep netcode. These tests assert:
//   1. same seed  => bit-identical Shard arrays (memcmp).
//   2. diff seed  => at least one shard differs.
//   3. total shard mass is conserved (== source mass within tolerance).

#include "physics/destruction/Fracture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstring>
#include <vector>

namespace dz = psynder::physics::destruction;
using dz::Shard;
using psynder::f32;
using psynder::u32;

namespace {

// Bitwise equality of two shard arrays. We compare raw bytes (not float ==) so
// the determinism guarantee is exact, including any sign/payload of zeros.
bool bitwise_equal(const std::vector<Shard>& a, const std::vector<Shard>& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(Shard)) == 0;
}

} // namespace

TEST_CASE("fracture is bit-identical for the same seed", "[destruction][fracture][determinism]") {
    const dz::FracturePattern pattern = dz::make_unit_box_pattern();
    REQUIRE(pattern.base_shards.size() == 8u);  // 2x2x2 grid

    std::vector<Shard> a;
    std::vector<Shard> b;
    dz::fracture(pattern, /*seed=*/0xC0FFEEull, a);
    dz::fracture(pattern, /*seed=*/0xC0FFEEull, b);

    REQUIRE(a.size() == pattern.base_shards.size());
    REQUIRE(bitwise_equal(a, b));
}

TEST_CASE("fracture differs for a different seed", "[destruction][fracture][determinism]") {
    const dz::FracturePattern pattern = dz::make_unit_box_pattern();

    std::vector<Shard> a;
    std::vector<Shard> b;
    dz::fracture(pattern, /*seed=*/1u, a);
    dz::fracture(pattern, /*seed=*/2u, b);

    REQUIRE(a.size() == b.size());
    REQUIRE_FALSE(bitwise_equal(a, b));
}

TEST_CASE("fracture conserves total mass", "[destruction][fracture][conservation]") {
    const f32 extents[3] = {2.0f, 0.5f, 1.5f};  // non-cubic, real metres
    const u32 divs[3] = {3u, 2u, 4u};            // 24 shards
    const f32 source_mass = 7.5f;                // kg

    const dz::FracturePattern pattern =
        dz::make_grid_pattern(extents, source_mass, divs);
    REQUIRE(pattern.base_shards.size() == 24u);

    std::vector<Shard> shards;
    dz::fracture(pattern, /*seed=*/0xBADC0DEull, shards);
    REQUIRE(shards.size() == 24u);

    f32 total = 0.0f;
    for (const Shard& s : shards) {
        REQUIRE(s.mass_kg > 0.0f);  // every shard has real positive mass
        total += s.mass_kg;
    }
    // Renormalisation guarantees conservation up to float round-off.
    REQUIRE(total == Catch::Approx(source_mass).epsilon(1e-4));
}
