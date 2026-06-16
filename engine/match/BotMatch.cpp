// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/BotMatch.cpp — see BotMatch.h. The deterministic combat AI that
// generates the per-client net inputs and drives the inner MatchSession.
//
// Determinism: id-ordered (ascending client) scans, strict FP, no RNG, reused
// per-tick input scratch. The bot reads the net authoritative world for combat
// geometry (that is what moves + what the hitreg ray-tests) and the ECS for
// liveness/team (the gameplay truth). The aim convention matches the net
// hitreg's register_fire_: dir = {cos(p)sin(y), -sin(p), cos(p)cos(y)} with yaw
// measured from +Z toward +X, so aiming straight at the target's net position
// drives the ray straight at it.

#include "match/BotMatch.h"

#include "gameplay/GameplayComponents.h"  // Health (liveness read)

#include "net/ReplicationSession.h"        // net::EntityState / kInputBtnFire

#include <cmath>

namespace psynder::match {

using gameplay::Health;

namespace {

// Re-derive the per-client team exactly as MatchSession's spawn_or_default does:
// an explicit spawn carries its own team; a missing spawn defaults to c % 2.
// MatchSession does NOT persist team as an ECS component, so BotMatch keeps its
// own copy captured at construction — this keeps the two consistent without
// touching MatchSession.
u32 team_for(std::span<const PlayerSpawn> spawns, u32 c) noexcept {
    if (c < spawns.size()) return spawns[c].team;
    return c % 2u;
}

constexpr f32 kDegToRad = 0.01745329252f;
constexpr f32 kRadToDeg = 57.29577951308f;

}  // namespace

BotMatch::BotMatch(const net::TickConfig& cfg, u32 client_count, u32 latency_ticks,
                   std::span<const PlayerSpawn> spawns)
    : session_(cfg, client_count, latency_ticks, spawns) {
    inputs_.resize(client_count);
    teams_.reserve(client_count);
    for (u32 c = 0; c < client_count; ++c) teams_.push_back(team_for(spawns, c));
}

void BotMatch::configure_match(const gameplay::MatchConfig& cfg,
                               std::span<const math::Vec3> spawn_points) {
    session_.configure_match(cfg, spawn_points);
}

int BotMatch::nearest_enemy_(u32 client,
                             std::span<const net::EntityState> world_net) noexcept {
    const u32 self_team = (client < teams_.size()) ? teams_[client] : 0u;

    // This bot's net position (id == client + 1). Defensive: bail if absent.
    const net::EntityState* self = nullptr;
    for (const net::EntityState& e : world_net) {
        if (e.id == client + 1u) { self = &e; break; }
    }
    if (self == nullptr) return -1;

    int  best_index = -1;
    f32  best_dist2 = 0.0f;
    // Ascending client-index scan with a strict < tie-break => the lowest client
    // index wins a tie, deterministically.
    for (u32 other = 0; other < session_.client_count(); ++other) {
        if (other == client) continue;
        if (teams_[other] == self_team) continue;  // never target a teammate
        if (!session_.alive(other)) continue;       // skip dead/respawning
        const Health* h = world().get<Health>(session_.player(other));
        if (h == nullptr || h->hp <= 0.0f) continue;

        // Find the enemy's net entity (id == other + 1).
        const net::EntityState* tgt = nullptr;
        for (const net::EntityState& e : world_net) {
            if (e.id == other + 1u) { tgt = &e; break; }
        }
        if (tgt == nullptr) continue;

        const f32 dx = tgt->pos[0] - self->pos[0];
        const f32 dy = tgt->pos[1] - self->pos[1];
        const f32 dz = tgt->pos[2] - self->pos[2];
        const f32 d2 = dx * dx + dy * dy + dz * dz;
        if (best_index < 0 || d2 < best_dist2) {
            best_dist2 = d2;
            best_index = static_cast<int>(other);
        }
    }
    return best_index;
}

net::Input BotMatch::build_input_(u32 client) noexcept {
    net::Input in{};  // do-nothing: no move, no fire, yaw 0

    // A dead bot emits no input (it is counting down to respawn).
    if (!session_.alive(client)) return in;

    const std::span<const net::EntityState> world_net = session_.net().authoritative();

    // This bot's net position.
    const net::EntityState* self = nullptr;
    for (const net::EntityState& e : world_net) {
        if (e.id == client + 1u) { self = &e; break; }
    }
    if (self == nullptr) return in;

    const int tgt_index = nearest_enemy_(client, world_net);
    if (tgt_index < 0) return in;  // no living enemy: idle this tick

    const net::EntityState* tgt = nullptr;
    for (const net::EntityState& e : world_net) {
        if (e.id == static_cast<u32>(tgt_index) + 1u) { tgt = &e; break; }
    }
    if (tgt == nullptr) return in;

    f32 dx = tgt->pos[0] - self->pos[0];
    f32 dy = tgt->pos[1] - self->pos[1];
    f32 dz = tgt->pos[2] - self->pos[2];
    f32 horiz2 = dx * dx + dz * dz;
    f32 dist2  = horiz2 + dy * dy;
    f32 dist   = std::sqrt(dist2);

    // Symmetry break: the net world seeds every entity at the origin (the ECS
    // spawn / respawn does NOT move the net entity — only step_entity does), so
    // two opposing bots can start (or respawn) coincident with nothing to chase.
    // When the target is within an epsilon, deterministically push apart along
    // +X / -X by client-index parity (even clients +X, odd -X) so the bots
    // separate, then the normal chase/aim/fire below engages. No RNG; the choice
    // is a pure function of the fixed client id.
    constexpr f32 kCoincidentEps = 0.25f;  // metres
    if (dist <= kCoincidentEps) {
        net::Input push{};
        push.move[0] = (client % 2u == 0u) ? ai_.move_speed_mps
                                           : -ai_.move_speed_mps;
        // Aim along the push so a stray shot still has a sane facing (it won't
        // fire: the target is not yet in front of us at this range).
        push.yaw_deg = (client % 2u == 0u) ? 90.0f : -90.0f;
        return push;
    }

    // ── Aim ── match register_fire_'s ray convention:
    //   dir = {cos(p)*sin(y), -sin(p), cos(p)*cos(y)}  (yaw 0 => +Z, +90 => +X)
    // so yaw = atan2(dx, dz) and pitch = -asin(dy / dist) aim straight at it.
    const f32 horiz = std::sqrt(horiz2);
    in.yaw_deg = std::atan2(dx, dz) * kRadToDeg;
    in.pitch_deg = (dist > 0.0f) ? (std::atan2(-dy, horiz) * kRadToDeg) : 0.0f;

    // ── Move ── velocity command toward the target while outside stop distance;
    // hold (zero move) once inside it so the bot doesn't run through the enemy.
    if (dist > ai_.stop_distance_m && dist > 0.0f) {
        const f32 inv = ai_.move_speed_mps / dist;
        in.move[0] = dx * inv;
        in.move[1] = dy * inv;
        in.move[2] = dz * inv;
    }

    // ── Fire ── once the target is within range. The bot always aims straight at
    // the target, so "roughly aimed" reduces to the range gate; the explicit
    // facing check below is kept for clarity + future spread/cone tuning.
    if (dist <= ai_.fire_range_m) {
        // Roughly-aimed test: the unit aim direction vs the unit to-target
        // direction. They are constructed from the same delta, so the dot is ~1;
        // gate above ~cos(2deg) to stay robust under float rounding.
        const f32 p = in.pitch_deg * kDegToRad;
        const f32 y = in.yaw_deg * kDegToRad;
        const f32 ax = std::cos(p) * std::sin(y);
        const f32 ay = -std::sin(p);
        const f32 az = std::cos(p) * std::cos(y);
        f32 facing = 0.0f;
        if (dist > 0.0f) {
            facing = (ax * dx + ay * dy + az * dz) / dist;
        }
        if (facing > 0.999f) {  // ~aligned (cos < ~2.5deg of error)
            in.buttons |= net::kInputBtnFire;
        }
    }

    return in;
}

void BotMatch::advance() {
    const u32 cc = session_.client_count();
    if (inputs_.size() != cc) inputs_.resize(cc);  // defensive; size is fixed

    // Ascending client index => deterministic build order. Each input reads the
    // net world snapshot from the END of the previous tick (the freshest
    // authoritative), the same snapshot for every client this tick.
    for (u32 c = 0; c < cc; ++c) inputs_[c] = build_input_(c);

    session_.advance(std::span<const net::Input>(inputs_.data(), cc));
}

}  // namespace psynder::match
