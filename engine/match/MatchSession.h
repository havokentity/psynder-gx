// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/MatchSession.h — the server-authoritative match loop that drives
// the REAL gameplay ECS through the netcode (DoD §8 bullet 1: "the net loop
// driving real gameplay", lag-compensated hitreg).
//
// This is the keystone bridge between two proven-but-disjoint subsystems:
//   * engine/net  — ReplicationSession: server-authoritative movement, client
//                   prediction + reconciliation over a deterministic latency
//                   channel, and lag-compensated hit DETECTION (it rewinds the
//                   world to the shooter's view-tick and emits HitEvents).
//   * engine/gameplay — Health / Score / Damage / respawn systems on scene::World.
//
// MatchSession owns a ReplicationSession (the transport + predict/reconcile +
// lag-comp ray test on net EntityState) AND a scene::World of one player Entity
// per client. Each advance():
//   1. ticks the net session (clients predict their own movement; the server
//      applies authoritative movement + registers lag-comp hits);
//   2. mirrors the authoritative net positions into the ECS TransformWS (so the
//      rest of the engine — agents / render / AoI — sees the canonical world);
//   3. APPLIES each newly-registered HitEvent through the real gameplay damage
//      path (damage_credited -> Health/Armor/Dead + Score), using the attacker's
//      ECS Weapon damage; a hit on an already-dead (respawning) player is
//      ignored, so there is no double-frag credit;
//   4. ticks respawns.
//
// Net entity ids are 1-based (id = client + 1; id 0 = invalid); the players_
// vector is the id<->Entity map (index = client = id - 1).
//
// Deterministic + headless (no GPU / audio / display) so it builds and runs
// under the dedicated-server preset. Authoritative lockstep tick: strict FP,
// stable id-ordered processing, no per-tick heap beyond a reused scratch vector.

#pragma once

#include "gameplay/GameplayComponents.h"
#include "net/Prediction.h"
#include "net/ReplicationSession.h"
#include "net/TickConfig.h"

#include "math/Math.h"

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::scene {
class World;
}

namespace psynder::match {

// Per-client spawn description used to seed the ECS players. Metric units.
struct PlayerSpawn {
    math::Vec3 pos{0.0f, 0.0f, 0.0f};
    u32        team = 0;
    f32        max_hp = 100.0f;
    f32        weapon_damage = 25.0f;  // damage applied per registered hit
    f32        respawn_delay_s = 3.0f;
};

class MatchSession {
public:
    // Construct an N-client match. `latency_ticks` is the one-way channel delay
    // (>= 0). Players spawn from `spawns` (size must equal client_count); a
    // shorter/empty span falls back to a default spawn for the missing clients.
    MatchSession(const net::TickConfig& cfg, u32 client_count, u32 latency_ticks,
                 std::span<const PlayerSpawn> spawns = {});

    ~MatchSession();

    MatchSession(const MatchSession&) = delete;
    MatchSession& operator=(const MatchSession&) = delete;

    // Advance one authoritative tick. `inputs` is one net::Input per client.
    void advance(std::span<const net::Input> inputs);

    u32 client_count() const noexcept { return repl_.client_count(); }
    u32 tick() const noexcept { return repl_.tick(); }

    // The ECS player entity for a client (the id<->Entity map; index = client).
    Entity player(u32 client) const noexcept { return players_[client]; }

    // Authoritative gameplay reads (server truth).
    f32  health(u32 client) const noexcept;
    u32  frags(u32 client) const noexcept;
    u32  deaths(u32 client) const noexcept;
    bool alive(u32 client) const noexcept;

    // Total lag-comp hits applied to the ECS so far (server-side, post-bridge).
    usize hits_applied() const noexcept { return hits_applied_; }

    // Underlying subsystems (for tests / higher layers).
    const net::ReplicationSession& net() const noexcept { return repl_; }
    scene::World& world() noexcept { return *world_; }
    const scene::World& world() const noexcept { return *world_; }

private:
    net::ReplicationSession repl_;
    // World is heap-owned via unique_ptr-free manual lifetime to keep this header
    // light (scene::World is forward-declared); managed in the .cpp.
    scene::World*       world_ = nullptr;
    std::vector<Entity> players_;          // index = client = net id - 1
    std::vector<Entity> respawn_scratch_;  // reused across ticks (no per-tick heap)
    usize               drained_hits_ = 0; // index into repl_.hit_events()
    usize               hits_applied_ = 0;
    f32                 dt_ = 0.0f;
};

}  // namespace psynder::match
