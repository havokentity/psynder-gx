// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/net/ReplicationSession.cpp — see ReplicationSession.h for the contract.

#include "net/ReplicationSession.h"

#include <utility>

namespace psynder::net {

void step_entity(EntityState& e, const Input& in, f32 dt) noexcept {
    e.pos[0] += in.move[0] * dt;
    e.pos[1] += in.move[1] * dt;
    e.pos[2] += in.move[2] * dt;
    e.yaw_deg = in.yaw_deg;
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
            } else {
                remain.push_back(m);
            }
        }
        c2s_.swap(remain);
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

}  // namespace psynder::net
