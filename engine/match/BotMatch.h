// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/BotMatch.h — a deterministic AI bot driver that PLAYS a
// MatchSession. Where MatchSession is the server-authoritative loop that
// consumes one net::Input per client per tick, BotMatch is the layer above it:
// each advance() it GENERATES those inputs from a simple, deterministic combat
// AI (move toward the nearest living enemy, fire when in range + aimed) and then
// drives the inner MatchSession with them. The result is a full bot-vs-bot match
// that plays out over the real netcode + gameplay path with no human inputs.
//
// Why drive the NET world (not the ECS world) for the AI read:
//   MatchSession moves players through engine/net's ReplicationSession. The net
//   authoritative entities (one EntityState per client, id = client + 1) are the
//   things that actually move (step_entity integrates Input.move) and that the
//   lag-compensated hitreg ray-tests (register_fire_ uses Input.yaw/pitch from
//   the shooter's net position). Net entities start at the origin regardless of
//   the ECS spawn, so the bot reads net positions to know where the fight is and
//   emits move/aim inputs in that same frame. Liveness + team come from the ECS
//   (Health / Team on players_[client]) — the authoritative gameplay truth the
//   damage path mutates. Combining the two gives a bot that targets a LIVING
//   ENEMY (ECS) and steers/shoots at its real location (net).
//
// Determinism (the match lane is lockstep): strict FP, ascending-client-index
// processing, NO RNG. Nearest-enemy ties break to the lowest client index. The
// per-tick input vector is reused scratch (no per-tick heap growth). Two
// BotMatch instances built + advanced identically produce a bit-identical match
// (frags / deaths / positions).

#pragma once

#include "match/MatchSession.h"

#include "net/Prediction.h"      // net::Input
#include "net/TickConfig.h"      // net::TickConfig

#include "gameplay/MatchRules.h"  // gameplay::MatchConfig / MatchPhase

#include "math/Math.h"
#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::match {

// Tunables for the bot combat AI. Metric units (1 unit = 1 metre). Defaults are
// chosen so a small free-for-all of bots closes the distance and trades frags
// within a few hundred ticks at 128 Hz. POD so it is trivially copyable and the
// same config reproduces the same match.
struct BotAiConfig {
    f32 move_speed_mps   = 30.0f;  // velocity command magnitude while chasing (m/s)
    f32 stop_distance_m  = 3.0f;   // hold position once this close to the target
    f32 fire_range_m     = 60.0f;  // only fire when the target is within this range
};

// A deterministic AI-driven bot match: a MatchSession whose per-client inputs
// are generated each tick by the combat AI. advance() builds one net::Input per
// client then ticks the inner session. All match reads forward to the session.
class BotMatch {
public:
    // Mirrors MatchSession's ctor: an N-client match over a fixed-latency channel
    // (latency_ticks >= 0), players seeded from `spawns` (size should equal
    // client_count; shorter/empty falls back to MatchSession's default spawns).
    BotMatch(const net::TickConfig& cfg, u32 client_count, u32 latency_ticks,
             std::span<const PlayerSpawn> spawns = {});

    BotMatch(const BotMatch&) = delete;
    BotMatch& operator=(const BotMatch&) = delete;

    // Forward to MatchSession::configure_match (rounds + win conditions +
    // deterministic respawn ring). Without it the bots still fight; the match
    // just never ends (no-limit rules). A non-empty spawn ring is recommended so
    // a frag drops the victim away from the shooter (anti-spawn-camp) — otherwise
    // both bots respawn on top of each other.
    void configure_match(const gameplay::MatchConfig& cfg,
                         std::span<const math::Vec3> spawn_points = {});

    // Override the combat-AI tunables (optional; defaults are sane). Call before
    // advance() for a reproducible run.
    void set_ai(const BotAiConfig& ai) noexcept { ai_ = ai; }
    const BotAiConfig& ai() const noexcept { return ai_; }

    // Advance one authoritative tick: build one net::Input per client from the
    // bot AI (read the net world + ECS liveness/team, pick nearest living enemy,
    // move toward it, fire when in range + aimed), then drive the inner session.
    void advance();

    // ── Reads forwarded to the inner MatchSession ──────────────────────────────
    u32 tick() const noexcept { return session_.tick(); }
    u32 client_count() const noexcept { return session_.client_count(); }

    f32  health(u32 client) const noexcept { return session_.health(client); }
    u32  frags(u32 client) const noexcept { return session_.frags(client); }
    u32  deaths(u32 client) const noexcept { return session_.deaths(client); }
    bool alive(u32 client) const noexcept { return session_.alive(client); }
    Entity player(u32 client) const noexcept { return session_.player(client); }

    usize hits_applied() const noexcept { return session_.hits_applied(); }

    gameplay::MatchPhase match_phase() const noexcept { return session_.match_phase(); }
    const gameplay::MatchState& match() const noexcept { return session_.match(); }
    Entity match_winner() const noexcept { return session_.match_winner(); }
    Entity winner() const noexcept { return session_.match_winner(); }  // alias
    u32 match_round() const noexcept { return session_.match_round(); }

    // Access to the inner subsystems (tests / higher layers).
    MatchSession& session() noexcept { return session_; }
    const MatchSession& session() const noexcept { return session_; }
    scene::World& world() noexcept { return session_.world(); }
    const scene::World& world() const noexcept { return session_.world(); }

private:
    // Build this client's Input for the current tick from the bot AI. Reads the
    // net authoritative world (positions) + the ECS (liveness/team). Returns a
    // do-nothing Input (no move, no fire) when the bot is dead or has no target.
    // Non-const: scene::World::get has no const overload (matching MatchSession's
    // own const reads going through the non-const world pointer); advance() (the
    // only caller) is non-const, so this is purely an internal access detail.
    net::Input build_input_(u32 client) noexcept;

    // The net-world index of the nearest LIVING ENEMY of `client`, or -1 when
    // none. Distance is in the net frame; ties break to the lowest client index
    // (ascending scan, strict <). Liveness + team come from the ECS.
    int nearest_enemy_(u32 client,
                       std::span<const net::EntityState> world_net) noexcept;

    MatchSession            session_;
    BotAiConfig             ai_{};
    std::vector<net::Input> inputs_;  // reused per-tick scratch (no per-tick heap)
    // Per-client team, captured at construction (MatchSession does NOT persist
    // team as an ECS component, so the bot keeps its own copy to filter targets).
    std::vector<u32>        teams_;
};

}  // namespace psynder::match
