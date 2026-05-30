// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/match_walkable_spawn.cpp — walkable-spawn validation in
// engine/match. Proves MatchSession::configure_terrain(h, min_walkable_updot)
// restricts the deterministic respawn-point selection to the WALKABLE subset of
// the spawn ring (world::outdoor::filter_walkable_spawns: the lockstep-safe
// slope gate). On a kill, the victim's chosen respawn is always a flat
// (walkable) point — never a steep cliff — and is still clamped onto the
// surface. The control case (the default min_walkable_updot == 0) reproduces the
// original unfiltered selection EXACTLY (a steep point CAN be chosen). A
// safety-fallback case proves an all-steep ring still yields a valid spawn (no
// crash, never leaves a player un-spawned), and a determinism case proves two
// identically-configured sessions pick the same respawn. Headless +
// deterministic; mirrors the construction/scenario of match_terrain_spawn.cpp.

#include "match/MatchSession.h"

#include "gameplay/GameplayComponents.h"
#include "gameplay/MatchRules.h"
#include "net/Prediction.h"
#include "net/TickConfig.h"

#include "world/outdoor/HeightfieldQuery.h"  // terrain_height (assertion oracle)
#include "world/outdoor/SpawnValidation.h"   // filter_walkable_spawns (oracle)
#include "world/outdoor/TerrainSlope.h"      // terrain_walkable (assertion oracle)
#include "world/outdoor/Terrain.h"           // HeightmapDesc

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::match;
using net::Input;
using net::kInputBtnFire;

namespace {

constexpr u32 kClients = 2;
constexpr u32 kLatency = 3;
constexpr u32 kSettle = 16;   // ticks for the victim to run onto the fire axis
constexpr u32 kTotal = 320;   // long enough for several kill/respawn cycles

// The walkable-slope gate: cos(max walkable angle). cos(45deg) ~ 0.707; the
// flat ring points clear it (updot 1.0), the steep ones fail it (updot ~0.22).
constexpr f32 kMinUpdot = 0.707f;

// Same two-player setup as match_terrain_spawn.cpp: shooter at origin, victim
// with a short respawn so the kill/respawn loop cycles within the test window.
std::array<PlayerSpawn, kClients> spawns() {
    std::array<PlayerSpawn, kClients> s{};
    s[0].pos = {0.0f, 0.0f, 0.0f};
    s[0].weapon_damage = 34.0f;   // 3 hits to drop 100 hp
    s[1].pos = {5.0f, 0.0f, 0.0f};
    s[1].respawn_delay_s = 0.1f;  // ~13 ticks at 128 Hz
    return s;
}

// Identical to the canonical scenario: client 0 aims +X (yaw 90) and fires once
// the victim has settled on the axis; client 1 runs +X onto the axis then idles.
Input input_for(u32 client, u32 tick) {
    Input in{};
    if (client == 0) {
        in.yaw_deg = 90.0f;
        if (tick >= kSettle + kLatency) in.buttons = kInputBtnFire;
    } else {
        in.move[0] = (tick < kSettle) ? 40.0f : 0.0f;
    }
    return in;
}

// 8x8 texels, 4 m spacing => covers [0, 28] m in X and Z. World mapping
// (Heightmap_internal.h): World X = column * spacing, World Z = row * spacing,
// World Y = u16 * scale.
constexpr u32 kSize = 8;
constexpr f32 kSpacing = 4.0f;
constexpr f32 kHeightScale = 0.01f;  // metres per u16 unit
constexpr u16 kFlatUnits = 2000u;    // 20 m flat plateau

// A terrain split along X: a FLAT plateau on the low-X half (columns 0..3,
// world X in [0, 12] m) and a STEEP ramp on the high-X half (columns 4..7,
// climbing ~18 m per column). With this field the low-X ring points are
// walkable (updot 1.0) and the high-X ring points are not (updot ~0.22 at the
// 0.707 gate). Kept ALIVE for the session's lifetime by the caller.
std::vector<u16> make_split_heights() {
    std::vector<u16> h(static_cast<usize>(kSize) * kSize, 0u);
    for (u32 z = 0; z < kSize; ++z) {
        for (u32 x = 0; x < kSize; ++x) {
            const u16 v = (x <= 3u)
                              ? kFlatUnits
                              : static_cast<u16>(kFlatUnits + (x - 3u) * 1800u);
            h[static_cast<usize>(z) * kSize + x] = v;
        }
    }
    return h;
}

// An ALL-STEEP terrain: the same steep ramp everywhere, so NO ring point is
// walkable and the safety fallback (use the full spawn set) must engage.
std::vector<u16> make_all_steep_heights() {
    std::vector<u16> h(static_cast<usize>(kSize) * kSize, 0u);
    for (u32 z = 0; z < kSize; ++z) {
        for (u32 x = 0; x < kSize; ++x) {
            h[static_cast<usize>(z) * kSize + x] =
                static_cast<u16>(kFlatUnits + x * 1800u);  // steep across all X
        }
    }
    return h;
}

world::outdoor::HeightmapDesc make_desc(const std::vector<u16>& heights) {
    world::outdoor::HeightmapDesc d{};
    d.size_x = kSize;
    d.size_z = kSize;
    d.spacing = kSpacing;
    d.height_scale = kHeightScale;
    d.heights = heights.data();  // non-owning; `heights` must outlive the session
    return d;
}

// Spawn ring at POSITIVE XZ. Two points sit on the FLAT half (X = 8) and two on
// the STEEP half (X = 20). The shooter sits near the origin, so the unfiltered
// "farthest from enemies" heuristic naturally favours the FAR (high-X, steep)
// points — exactly the points the walkable gate must reject.
std::array<math::Vec3, 4> spawn_ring() {
    return {math::Vec3{8.0f, 0.0f, 8.0f}, math::Vec3{20.0f, 0.0f, 8.0f},
            math::Vec3{8.0f, 0.0f, 20.0f}, math::Vec3{20.0f, 0.0f, 20.0f}};
}

gameplay::MatchConfig match_cfg() {
    gameplay::MatchConfig mc{};
    mc.frag_limit = 2u;
    mc.warmup_s = 8.0f / 128.0f;  // ~8 ticks of warmup (no damage)
    mc.intermission_s = 0.25f;
    return mc;
}

// How the terrain opt-in is configured for a run.
enum class Mode {
    kNoTerrain,   // no configure_terrain at all (pre-feature control)
    kClampOnly,   // configure_terrain(h) — clamp, NO walkable gate (default 0)
    kWalkable,    // configure_terrain(h, kMinUpdot) — clamp + walkable gate
};

struct Outcome {
    bool       ended = false;
    math::Vec3 victim_spawn{0.0f, 0.0f, 0.0f};
};

// Run the canonical kill/respawn scenario to the first ended round. The
// heightmap data is owned by the caller (held alive across the run).
Outcome run(Mode mode, const std::vector<u16>& heights) {
    const net::TickConfig cfg = net::tick_config_128();
    const auto sp = spawns();
    MatchSession m(cfg, kClients, kLatency, std::span<const PlayerSpawn>(sp));

    const auto ring = spawn_ring();
    m.configure_match(match_cfg(), std::span<const math::Vec3>(ring));

    switch (mode) {
        case Mode::kNoTerrain:
            REQUIRE_FALSE(m.has_terrain());
            REQUIRE(m.min_walkable_updot() == Catch::Approx(0.0f));
            break;
        case Mode::kClampOnly:
            m.configure_terrain(make_desc(heights));  // default gate (0 => off)
            REQUIRE(m.has_terrain());
            REQUIRE(m.min_walkable_updot() == Catch::Approx(0.0f));
            break;
        case Mode::kWalkable:
            m.configure_terrain(make_desc(heights), kMinUpdot);
            REQUIRE(m.has_terrain());
            REQUIRE(m.min_walkable_updot() == Catch::Approx(kMinUpdot));
            break;
    }

    Outcome out;
    for (u32 t = 0; t < kTotal; ++t) {
        std::array<Input, kClients> ins;
        for (u32 c = 0; c < kClients; ++c) ins[c] = input_for(c, t);
        m.advance(std::span<const Input>(ins.data(), kClients));
        if (m.match().just_ended) {
            out.ended = true;
            if (const auto* r = m.world().get<gameplay::Respawnable>(m.player(1))) {
                out.victim_spawn = r->spawn_pos;
            }
            break;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("match-walkable: the gate keeps the victim respawn on walkable ground",
          "[match][gameplay]") {
    const std::vector<u16> heights = make_split_heights();  // KEEP ALIVE
    const world::outdoor::HeightmapDesc desc = make_desc(heights);

    const Outcome o = run(Mode::kWalkable, heights);
    REQUIRE(o.ended);

    // The chosen respawn must be a WALKABLE point: its ground slope clears the
    // gate (it landed on the flat plateau, never the steep cliff).
    REQUIRE(world::outdoor::terrain_walkable(desc, o.victim_spawn.x,
                                             o.victim_spawn.z, kMinUpdot));
    // Concretely that means the FLAT half (the steep ring points sit at X = 20).
    REQUIRE(o.victim_spawn.x == Catch::Approx(8.0f));

    // And it is still clamped onto the surface (Y == terrain_height at its XZ).
    const f32 ground =
        world::outdoor::terrain_height(desc, o.victim_spawn.x, o.victim_spawn.z);
    REQUIRE(o.victim_spawn.y == Catch::Approx(ground));
    REQUIRE(ground > 0.0f);  // the clamp did real work (plateau is at 20 m)
}

TEST_CASE("match-walkable: without the gate the unfiltered selection can pick a steep point",
          "[match][gameplay]") {
    const std::vector<u16> heights = make_split_heights();
    const world::outdoor::HeightmapDesc desc = make_desc(heights);

    // configure_terrain(h) with the DEFAULT gate (0) must be byte-identical to
    // the pre-feature behaviour: the unfiltered "farthest from enemies"
    // heuristic, which here favours the FAR (steep, X = 20) points.
    const Outcome clamp_only = run(Mode::kClampOnly, heights);
    REQUIRE(clamp_only.ended);

    // The unfiltered pick landed on a STEEP point — exactly the point the gate
    // rejects — proving the default path does NOT filter.
    REQUIRE(clamp_only.victim_spawn.x == Catch::Approx(20.0f));
    REQUIRE_FALSE(world::outdoor::terrain_walkable(
        desc, clamp_only.victim_spawn.x, clamp_only.victim_spawn.z, kMinUpdot));

    // It is still surface-clamped (the clamp is independent of the gate).
    const f32 ground = world::outdoor::terrain_height(
        desc, clamp_only.victim_spawn.x, clamp_only.victim_spawn.z);
    REQUIRE(clamp_only.victim_spawn.y == Catch::Approx(ground));

    // The walkable gate moves the pick to a DIFFERENT (flat) XZ than the
    // unfiltered selection — the feature genuinely changes the chosen spawn.
    const Outcome gated = run(Mode::kWalkable, heights);
    REQUIRE(gated.ended);
    REQUIRE(gated.victim_spawn.x != Catch::Approx(clamp_only.victim_spawn.x));
}

TEST_CASE("match-walkable: an all-steep ring falls back to a valid spawn",
          "[match][gameplay]") {
    const std::vector<u16> heights = make_all_steep_heights();  // KEEP ALIVE
    const world::outdoor::HeightmapDesc desc = make_desc(heights);

    // No ring point is walkable, so filter_walkable_spawns yields the empty set.
    const auto ring = spawn_ring();
    REQUIRE_FALSE(world::outdoor::any_walkable_spawn(
        desc, std::span<const math::Vec3>(ring), kMinUpdot));

    // The gate must NOT leave the victim un-spawned: it falls back to the full
    // ring (same as the no-gate selection) and still clamps onto the surface.
    const Outcome gated = run(Mode::kWalkable, heights);
    REQUIRE(gated.ended);

    // The fallback picks one of the ring points (positive XZ) and clamps it.
    REQUIRE(gated.victim_spawn.x > 0.0f);
    REQUIRE(gated.victim_spawn.z > 0.0f);
    const f32 ground =
        world::outdoor::terrain_height(desc, gated.victim_spawn.x, gated.victim_spawn.z);
    REQUIRE(gated.victim_spawn.y == Catch::Approx(ground));

    // And it matches the unfiltered selection (the fallback IS the full set).
    const Outcome clamp_only = run(Mode::kClampOnly, heights);
    REQUIRE(clamp_only.ended);
    REQUIRE(gated.victim_spawn.x == Catch::Approx(clamp_only.victim_spawn.x));
    REQUIRE(gated.victim_spawn.z == Catch::Approx(clamp_only.victim_spawn.z));
}

TEST_CASE("match-walkable: identical configured sessions pick the same respawn",
          "[match][gameplay]") {
    const std::vector<u16> heights = make_split_heights();

    const Outcome a = run(Mode::kWalkable, heights);
    const Outcome b = run(Mode::kWalkable, heights);
    REQUIRE(a.ended);
    REQUIRE(b.ended);

    // Deterministic: bit-identical respawn across two identical runs.
    REQUIRE(a.victim_spawn.x == Catch::Approx(b.victim_spawn.x));
    REQUIRE(a.victim_spawn.y == Catch::Approx(b.victim_spawn.y));
    REQUIRE(a.victim_spawn.z == Catch::Approx(b.victim_spawn.z));
}
