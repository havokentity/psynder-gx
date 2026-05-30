// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/match_terrain_spawn.cpp — terrain-aware respawns in engine/match.
// Proves MatchSession::configure_terrain makes the deterministic spawn-point
// selection terrain-aware: on a kill, the victim's chosen respawn point has its
// Y clamped onto the heightmap surface (world::outdoor::clamp_to_ground), so a
// frag on a hilly outdoor map lands the victim ON the ground instead of at the
// raw spawn-point Y. The control case (no configure_terrain) reproduces the
// original behaviour exactly. Headless + deterministic; mirrors the
// construction/scenario of match_session.cpp.

#include "match/MatchSession.h"

#include "gameplay/GameplayComponents.h"
#include "gameplay/MatchRules.h"
#include "net/Prediction.h"
#include "net/TickConfig.h"

#include "world/outdoor/HeightfieldQuery.h"  // terrain_height (assertion oracle)
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

// Same two-player setup as match_session.cpp: shooter at origin, victim with a
// short respawn so the kill/respawn loop cycles within the test window.
std::array<PlayerSpawn, kClients> spawns() {
    std::array<PlayerSpawn, kClients> s{};
    s[0].pos = {0.0f, 0.0f, 0.0f};
    s[0].weapon_damage = 34.0f;   // 3 hits to drop 100 hp
    s[1].pos = {5.0f, 0.0f, 0.0f};
    s[1].respawn_delay_s = 0.1f;  // ~13 ticks at 128 Hz
    return s;
}

// Client 0 aims +X (yaw 90) and fires once the victim has settled on the axis;
// client 1 runs +X onto the axis then idles so every shot connects. Identical
// to the canonical match_session.cpp scenario.
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

// A small non-flat heightmap. World mapping (Heightmap_internal.h):
//   World X = column * spacing, World Z = row * spacing, World Y = u16 * scale.
// The spawn ring below sits at positive XZ inside this grid so the chosen spawn
// samples REAL (non-zero, sloped) terrain rather than the flat out-of-bounds
// border. 8x8 texels, 4 m spacing => covers [0, 28] m in X and Z.
constexpr u32 kSize = 8;
constexpr f32 kSpacing = 4.0f;
constexpr f32 kHeightScale = 0.01f;  // metres per u16 unit

// Build a non-flat field: height varies with both column and row so distinct XZ
// spawns get distinct ground heights. Kept ALIVE for the session's lifetime by
// the caller (returned by value into a test-scope vector).
std::vector<u16> make_heights() {
    std::vector<u16> h(static_cast<usize>(kSize) * kSize, 0u);
    for (u32 z = 0; z < kSize; ++z) {
        for (u32 x = 0; x < kSize; ++x) {
            // Deterministic ramp + bump: clearly non-flat, all distinct.
            const u32 v = 1000u + x * 500u + z * 300u + (x * z) * 50u;
            h[static_cast<usize>(z) * kSize + x] = static_cast<u16>(v);
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

// Spawn ring at POSITIVE XZ so every candidate lands on the live terrain. The
// farthest-from-origin candidate is what the heuristic deterministically picks.
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

// Result of running the scenario to the first ended round.
struct Outcome {
    bool       ended = false;
    math::Vec3 victim_spawn{0.0f, 0.0f, 0.0f};
};

// `with_terrain` toggles the opt-in terrain clamp. The heightmap data is owned
// by the caller (held alive across the whole run via `heights`).
Outcome run(bool with_terrain, const std::vector<u16>& heights) {
    const net::TickConfig cfg = net::tick_config_128();
    const auto sp = spawns();
    MatchSession m(cfg, kClients, kLatency, std::span<const PlayerSpawn>(sp));

    const auto ring = spawn_ring();
    m.configure_match(match_cfg(), std::span<const math::Vec3>(ring));

    if (with_terrain) {
        m.configure_terrain(make_desc(heights));
        REQUIRE(m.has_terrain());
    } else {
        REQUIRE_FALSE(m.has_terrain());
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

TEST_CASE("match-terrain: a configured terrain clamps the victim respawn onto the surface",
          "[match][gameplay]") {
    const std::vector<u16> heights = make_heights();  // KEEP ALIVE for the session
    const world::outdoor::HeightmapDesc desc = make_desc(heights);

    const Outcome o = run(/*with_terrain=*/true, heights);
    REQUIRE(o.ended);

    // The chosen spawn came from the positive-XZ ring (X and Z snapped to a
    // ring point), so it sampled real terrain.
    REQUIRE(o.victim_spawn.x > 0.0f);
    REQUIRE(o.victim_spawn.z > 0.0f);

    // Its Y was clamped onto the surface: spawn_pos.y == terrain_height(XZ).
    const f32 ground =
        world::outdoor::terrain_height(desc, o.victim_spawn.x, o.victim_spawn.z);
    REQUIRE(o.victim_spawn.y == Catch::Approx(ground));
    // And the terrain is genuinely non-flat there (the clamp did real work).
    REQUIRE(ground > 0.0f);
}

TEST_CASE("match-terrain: without configure_terrain the respawn keeps the raw spawn Y",
          "[match][gameplay]") {
    const std::vector<u16> heights = make_heights();

    const Outcome o = run(/*with_terrain=*/false, heights);
    REQUIRE(o.ended);

    // The ring points all have Y == 0; without the terrain clamp the victim
    // respawns at the raw spawn Y, exactly as before this feature existed.
    REQUIRE(o.victim_spawn.x > 0.0f);
    REQUIRE(o.victim_spawn.z > 0.0f);
    REQUIRE(o.victim_spawn.y == Catch::Approx(0.0f));
}

TEST_CASE("match-terrain: terrain clamp picks the same XZ but raises only the Y",
          "[match][gameplay]") {
    const std::vector<u16> heights = make_heights();
    const world::outdoor::HeightmapDesc desc = make_desc(heights);

    const Outcome flat = run(/*with_terrain=*/false, heights);
    const Outcome clamped = run(/*with_terrain=*/true, heights);
    REQUIRE(flat.ended);
    REQUIRE(clamped.ended);

    // The deterministic spawn SELECTION (XZ) is unchanged by the terrain opt-in;
    // only the Y differs — proving terrain-awareness is purely a surface clamp.
    REQUIRE(clamped.victim_spawn.x == Catch::Approx(flat.victim_spawn.x));
    REQUIRE(clamped.victim_spawn.z == Catch::Approx(flat.victim_spawn.z));
    REQUIRE(flat.victim_spawn.y == Catch::Approx(0.0f));
    REQUIRE(clamped.victim_spawn.y == Catch::Approx(world::outdoor::terrain_height(
                                          desc, clamped.victim_spawn.x,
                                          clamped.victim_spawn.z)));
    REQUIRE(clamped.victim_spawn.y > flat.victim_spawn.y);
}
