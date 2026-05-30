// SPDX-License-Identifier: MIT OR Apache-2.0
//
// PsyServerGX — the headless dedicated-server binary. Runs the
// server-authoritative match loop (engine/match MatchSession: netcode movement
// prediction/reconciliation + lag-compensated hitreg driving the real gameplay
// ECS) on a fixed 128 Hz tick, with NO graphics / audio / editor. This is the
// concrete realization of DoD §8 bullet 1 ("runs server-authoritative") and the
// artifact the dedicated-server build (PSYNDER_GX_DEDICATED_SERVER) produces.
//
// Until the UDP transport is bound (follow-up), the connected clients are
// stood in for by a deterministic scripted input schedule, so a headless smoke
// (`PsyServerGX --ticks=N`) exercises the entire server tick path and exits 0.
// Deterministic: fixed tick, strict FP, no wall-clock in the state path.

#include "match/MatchSession.h"

#include "gameplay/MatchRules.h"
#include "net/Prediction.h"
#include "net/TickConfig.h"

#include "math/Math.h"
#include "core/Types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

using namespace psynder;

namespace {

// Parse `--key=<u32>` from argv; returns `def` if absent. No exceptions (avoids
// std::stoi throw) so the server start path stays allocation/throw-free.
u32 arg_u32(int argc, char** argv, const char* key, u32 def) {
    const std::size_t klen = std::strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], key, klen) == 0) {
            return static_cast<u32>(std::strtoul(argv[i] + klen, nullptr, 10));
        }
    }
    return def;
}

}  // namespace

namespace {
const char* phase_name(gameplay::MatchPhase p) {
    switch (p) {
        case gameplay::MatchPhase::Warmup: return "warmup";
        case gameplay::MatchPhase::Active: return "active";
        case gameplay::MatchPhase::Intermission: return "intermission";
    }
    return "?";
}
}  // namespace

int main(int argc, char** argv) {
    const u32 ticks = arg_u32(argc, argv, "--ticks=", 256u);
    const u32 clients = arg_u32(argc, argv, "--clients=", 2u);
    const u32 latency = arg_u32(argc, argv, "--latency=", 3u);
    const u32 frag_limit = arg_u32(argc, argv, "--frag-limit=", 3u);
    const u32 warmup_ticks = arg_u32(argc, argv, "--warmup-ticks=", 8u);

    std::printf(
        "[psyserver] headless dedicated server — clients=%u latency=%u ticks=%u "
        "tickrate=128 frag_limit=%u\n",
        clients, latency, ticks, frag_limit);

    if (clients == 0u) {
        std::printf("[psyserver] no clients — nothing to simulate\n");
        return 0;
    }

    const net::TickConfig cfg = net::tick_config_128();
    const f32 dt = static_cast<f32>(cfg.frame_sec);

    // Spawn scripted players. Client 0 is the shooter (aims +X, holds fire);
    // the rest run onto the fire axis then hold — stand-ins for connected
    // clients until the UDP transport lands. Server-authoritative + deterministic.
    std::vector<match::PlayerSpawn> spawns(clients);
    for (u32 c = 0; c < clients; ++c) {
        spawns[c].pos = {static_cast<f32>(c) * 4.0f, 0.0f, 0.0f};
        spawns[c].respawn_delay_s = 0.5f;
    }
    match::MatchSession server(cfg, clients, latency,
                               std::span<const match::PlayerSpawn>(spawns));

    // Run a real match: a warmup, a frag limit, and a ring of spawn points the
    // server picks from (farthest-from-enemy) on each death.
    gameplay::MatchConfig mcfg{};
    mcfg.frag_limit = frag_limit;
    mcfg.warmup_s = static_cast<f32>(warmup_ticks) * dt;
    mcfg.intermission_s = 1.0f;
    const std::vector<math::Vec3> spawn_points = {
        {-8.0f, 0.0f, -8.0f}, {8.0f, 0.0f, -8.0f},
        {-8.0f, 0.0f, 8.0f},  {8.0f, 0.0f, 8.0f}};
    server.configure_match(mcfg, std::span<const math::Vec3>(spawn_points));

    gameplay::MatchPhase last_phase = gameplay::MatchPhase::Warmup;
    std::printf("[psyserver] phase -> %s\n", phase_name(last_phase));

    std::vector<net::Input> inputs(clients);
    for (u32 t = 0; t < ticks; ++t) {
        for (u32 c = 0; c < clients; ++c) {
            net::Input in{};
            if (c == 0u) {
                in.yaw_deg = 90.0f;  // aim +X
                in.buttons = net::kInputBtnFire;
            } else {
                in.move[0] = (t < 16u) ? 40.0f : 0.0f;  // run onto axis, then hold
            }
            inputs[c] = in;
        }
        server.advance(std::span<const net::Input>(inputs.data(), clients));
        if (server.match_phase() != last_phase) {
            last_phase = server.match_phase();
            std::printf("[psyserver] tick %4u  phase -> %s (round %u)\n", t,
                        phase_name(last_phase), server.match_round());
        }
        if (server.match().just_ended) {
            const Entity w = server.match_winner();
            std::printf("[psyserver] tick %4u  MATCH OVER — winner entity %llu\n",
                        t, static_cast<unsigned long long>(w.raw));
        }
        if ((t % 64u) == 0u) {
            std::printf("[psyserver] tick %4u  hits_applied=%zu phase=%s\n", t,
                        server.hits_applied(), phase_name(server.match_phase()));
        }
    }

    std::printf("[psyserver] final scoreboard:\n");
    unsigned long long total_frags = 0;
    for (u32 c = 0; c < clients; ++c) {
        std::printf("  client %u: hp=%3.0f frags=%u deaths=%u\n", c,
                    static_cast<double>(server.health(c)), server.frags(c),
                    server.deaths(c));
        total_frags += server.frags(c);
    }
    std::printf("[psyserver] complete: %u ticks, %zu hits applied, %llu frags\n",
                ticks, server.hits_applied(), total_frags);
    return 0;
}
