// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/MatchSession.cpp — see MatchSession.h. The authoritative bridge
// that runs the real gameplay damage path off the netcode's lag-compensated hit
// detection. Deterministic: id-ordered, strict FP, reused scratch.

#include "match/MatchSession.h"

#include "gameplay/Damage.h"
#include "gameplay/GameplayComponents.h"

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include "math/Math.h"

namespace psynder::match {

using gameplay::Dead;
using gameplay::Health;
using gameplay::Respawnable;
using gameplay::Score;
using gameplay::Weapon;
using scene::TransformWS;

namespace {
PlayerSpawn spawn_or_default(std::span<const PlayerSpawn> spawns, u32 c) {
    if (c < spawns.size()) return spawns[c];
    // Default: line players up along X so distinct clients sit at distinct
    // deterministic positions even without an explicit spawn table.
    PlayerSpawn s{};
    s.pos = {static_cast<f32>(c) * 2.0f, 0.0f, 0.0f};
    s.team = c % 2u;
    return s;
}
}  // namespace

MatchSession::MatchSession(const net::TickConfig& cfg, u32 client_count,
                           u32 latency_ticks, std::span<const PlayerSpawn> spawns)
    : repl_(cfg, client_count, latency_ticks),
      dt_(static_cast<f32>(cfg.frame_sec)) {
    world_ = new scene::World();
    players_.reserve(client_count);
    for (u32 c = 0; c < client_count; ++c) {
        const PlayerSpawn s = spawn_or_default(spawns, c);
        const Entity e = world_->create();

        TransformWS t{};
        t.mtw = math::translate(s.pos);
        t.prev_mtw = t.mtw;
        world_->add(e, t);

        world_->add(e, Health{s.max_hp, s.max_hp});
        world_->add(e, Score{0u, 0u});
        world_->add(e, Respawnable{s.pos, s.respawn_delay_s});
        // ammo < 0 => infinite; fire gating happens on the net/client side, the
        // server applies the registered hit's damage straight from this Weapon.
        world_->add(e, Weapon{s.weapon_damage, 0.0f, 0.0f, -1, 0.5f});

        players_.push_back(e);
    }
}

MatchSession::~MatchSession() { delete world_; }

void MatchSession::advance(std::span<const net::Input> inputs) {
    // (1) Net tick: client prediction + server-authoritative movement + lag-comp
    //     hit DETECTION (HitEvents appended to repl_.hit_events()).
    repl_.advance(inputs);

    // (2) Mirror the authoritative net positions into the ECS so the rest of the
    //     engine reads the canonical world. id == client + 1.
    for (const net::EntityState& es : repl_.authoritative()) {
        if (es.id == 0u) continue;
        const u32 client = es.id - 1u;
        if (client >= players_.size()) continue;
        if (TransformWS* t = world_->get<TransformWS>(players_[client])) {
            t->prev_mtw = t->mtw;
            t->mtw = math::translate({es.pos[0], es.pos[1], es.pos[2]});
        }
    }

    // (3) Apply newly-registered lag-comp hits through the REAL gameplay damage
    //     path. Drain only the events appended since last advance.
    const std::vector<net::HitEvent>& hits = repl_.hit_events();
    for (usize i = drained_hits_; i < hits.size(); ++i) {
        const net::HitEvent& h = hits[i];
        if (h.victim == 0u) continue;  // miss
        const u32 a = h.attacker - 1u;
        const u32 v = h.victim - 1u;
        if (a >= players_.size() || v >= players_.size()) continue;
        const Entity attacker = players_[a];
        const Entity victim = players_[v];
        // No double-credit: a player already dead (counting down to respawn)
        // cannot be killed again. Deterministic (state, not timing).
        if (world_->get<Dead>(victim) != nullptr) continue;
        f32 dmg = 25.0f;
        if (const Weapon* wp = world_->get<Weapon>(attacker)) dmg = wp->damage;
        gameplay::damage_credited(*world_, attacker, victim, dmg);
        ++hits_applied_;
    }
    drained_hits_ = hits.size();

    // (4) Respawn timers (id-ordered, reused scratch).
    gameplay::update_respawns(*world_, dt_, respawn_scratch_);
}

f32 MatchSession::health(u32 client) const noexcept {
    const Health* h = world_->get<Health>(players_[client]);
    return h ? h->hp : 0.0f;
}

u32 MatchSession::frags(u32 client) const noexcept {
    const Score* s = world_->get<Score>(players_[client]);
    return s ? s->frags : 0u;
}

u32 MatchSession::deaths(u32 client) const noexcept {
    const Score* s = world_->get<Score>(players_[client]);
    return s ? s->deaths : 0u;
}

bool MatchSession::alive(u32 client) const noexcept {
    return world_->get<Dead>(players_[client]) == nullptr;
}

}  // namespace psynder::match
