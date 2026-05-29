// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/net/ReplicationSession.cpp — see ReplicationSession.h for the contract.

#include "net/ReplicationSession.h"

#include <cmath>
#include <utility>

namespace psynder::net {

void step_entity(EntityState& e, const Input& in, f32 dt) noexcept {
    e.pos[0] += in.move[0] * dt;
    e.pos[1] += in.move[1] * dt;
    e.pos[2] += in.move[2] * dt;
    e.yaw_deg = in.yaw_deg;
}

u32 raycast_nearest_entity(std::span<const EntityState> world, u32 exclude_id,
                           const f32 origin[3], const f32 dir[3], f32 radius,
                           f32 max_t) noexcept {
    const f32 dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (dl <= 0.0f) return 0u;
    const f32 d[3] = {dir[0] / dl, dir[1] / dl, dir[2] / dl};
    u32 best = 0u;
    f32 best_t = max_t;
    for (const EntityState& e : world) {
        if (e.id == exclude_id || e.id == 0u) continue;
        const f32 oc[3] = {origin[0] - e.pos[0], origin[1] - e.pos[1],
                           origin[2] - e.pos[2]};
        const f32 b = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
        const f32 c = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - radius * radius;
        const f32 disc = b * b - c;
        if (disc < 0.0f) continue;
        const f32 sq = std::sqrt(disc);
        f32 t = -b - sq;          // near intersection
        if (t < 0.0f) t = -b + sq;  // origin inside the sphere -> far root
        if (t >= 0.0f && t < best_t) {
            best_t = t;
            best = e.id;
        }
    }
    return best;
}

ReplicationSession::ReplicationSession(const TickConfig& cfg, u32 client_count,
                                       u32 latency_ticks)
    : dt_(static_cast<f32>(cfg.frame_sec)),
      latency_(latency_ticks),
      server_world_(client_count),
      server_last_input_(client_count, 0u),
      clients_(client_count),
      scratch_(256) {
    for (u32 c = 0; c < client_count; ++c) {
        server_world_[c] = EntityState{};
        server_world_[c].id = c + 1u;  // entity id 0 is reserved "invalid"
        clients_[c].predicted = server_world_[c];
    }
}

void ReplicationSession::advance(std::span<const Input> inputs) {
    const u32 t = tick_;
    const u32 cc = client_count();

    // (1) Clients: stamp this tick, record locally, predict immediately, send up.
    for (u32 c = 0; c < cc; ++c) {
        Input in = inputs[c];
        // 1-based input ticks: server_last_input_ starts at 0 meaning "nothing
        // acked yet", so reconcile (replay tick > acked) replays ALL inputs
        // during the first `latency` ticks instead of dropping the tick-0 input.
        in.tick = t + 1u;
        clients_[c].ring.push(in);
        step_entity(clients_[c].predicted, in, dt_);  // local prediction
        c2s_.push_back(C2S{t + latency_, c, in});
    }

    // (2) Server: apply the inputs that arrive this tick (each exactly once, in
    //     stored order = ascending send-tick then client), then snapshot.
    {
        std::vector<C2S> remain;
        remain.reserve(c2s_.size());
        for (const C2S& m : c2s_) {
            if (m.deliver_tick <= t) {
                step_entity(server_world_[m.client], m.input, dt_);
                server_last_input_[m.client] = m.input.tick;
                if ((m.input.buttons & kInputBtnFire) != 0u) {
                    register_fire_(t, m.client, m.input);
                }
            } else {
                remain.push_back(m);
            }
        }
        c2s_.swap(remain);

        // Record this tick's post-apply authoritative world for lag-comp rewind,
        // trimmed to a window comfortably past the round-trip view delay.
        history_.push_back({t, server_world_});
        const usize window = static_cast<usize>(2u * latency_) + 8u;
        while (history_.size() > window) history_.erase(history_.begin());
    }
    {
        // Delta-encode the authoritative snapshot against the previously-sent one
        // (full snapshot on the first tick). The in-order lossless channel keeps
        // every client's applied baseline equal to this `server_prev_`.
        std::vector<u8> delta;
        const std::span<const EntityState> base =
            server_has_prev_ ? std::span<const EntityState>(server_prev_)
                             : std::span<const EntityState>();
        encode_delta(base, std::span<const EntityState>(server_world_), delta);
        last_delta_bytes_ = delta.size();
        for (u32 c = 0; c < cc; ++c) {
            s2c_.push_back(S2C{t + latency_, c, t, server_last_input_[c], delta});
        }
        server_prev_ = server_world_;
        server_has_prev_ = true;
    }

    // (3) Clients: apply the snapshots that arrive this tick, then reconcile —
    //     snap the own entity to authoritative and replay still-unacked inputs.
    {
        const auto apply = [this](EntityState& s, const Input& in) {
            step_entity(s, in, dt_);
        };
        std::vector<S2C> remain;
        remain.reserve(s2c_.size());
        for (S2C& m : s2c_) {
            if (m.deliver_tick > t) {
                remain.push_back(std::move(m));
                continue;
            }
            ClientState& cs = clients_[m.client];
            std::vector<EntityState> full;
            const std::span<const EntityState> base =
                cs.has_baseline ? std::span<const EntityState>(cs.baseline)
                                : std::span<const EntityState>();
            if (!apply_delta(base, std::span<const u8>(m.delta), full)) {
                continue;  // truncated — cannot happen on the lossless channel
            }
            cs.baseline = full;
            cs.has_baseline = true;
            cs.last_applied_server_tick = m.server_tick;
            for (const EntityState& e : full) {
                if (e.id == m.client + 1u) {
                    cs.predicted = e;  // snap to authoritative
                    break;
                }
            }
            predict_present(cs.ring, m.acked_input_tick, cs.predicted,
                            std::span<Input>(scratch_), apply);
            cs.ring.drop_acked(m.acked_input_tick);
        }
        s2c_.swap(remain);
    }

    ++tick_;
}

void ReplicationSession::register_fire_(u32 server_tick, u32 client,
                                        const Input& in) {
    // The shooter fired against what it SAW: the snapshot it had at send time,
    // which is the authoritative world from ~one round trip ago.
    const u32 view_tick =
        (server_tick >= 2u * latency_) ? (server_tick - 2u * latency_) : 0u;
    const std::vector<EntityState>* snap = nullptr;
    for (const auto& h : history_) {
        if (h.first == view_tick) {
            snap = &h.second;
            break;
        }
    }
    if (snap == nullptr) return;  // outside the rewind window

    const u32 shooter_id = client + 1u;
    const EntityState* sh = nullptr;
    for (const EntityState& e : *snap) {
        if (e.id == shooter_id) {
            sh = &e;
            break;
        }
    }
    if (sh == nullptr) return;

    constexpr f32 kDegToRad = 0.01745329252f;
    const f32 yaw = in.yaw_deg * kDegToRad;
    const f32 pitch = in.pitch_deg * kDegToRad;
    const f32 dir[3] = {std::cos(pitch) * std::sin(yaw), -std::sin(pitch),
                        std::cos(pitch) * std::cos(yaw)};
    const u32 victim = raycast_nearest_entity(
        std::span<const EntityState>(*snap), shooter_id, sh->pos, dir,
        /*radius=*/0.5f, /*max_t=*/1000.0f);
    if (victim != 0u) {
        hit_events_.push_back(HitEvent{shooter_id, victim, server_tick});
    }
}

}  // namespace psynder::net
