// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/net/ReplicationSession.h
//
// The server-authoritative replication loop (the netcode keystone): it ties
// SnapshotReplication (#40) + client Prediction/reconciliation (#41) into one
// deterministic vertical slice.
//
//   - The SERVER owns the authoritative world (one EntityState per client). Each
//     tick it applies the client inputs that arrived this tick, delta-encodes the
//     snapshot against the previously-sent one, and ships it to every client.
//   - Each CLIENT pushes its local input, predicts immediately (integrates its
//     own entity), and sends the input upstream. On each authoritative snapshot
//     it snaps its own entity to the server value and replays the still-unacked
//     inputs (predict_present), so a correct prediction is invisible and a wrong
//     one is corrected.
//
// Transport is a DETERMINISTIC fixed-latency in-process channel (no wall-clock,
// no sockets, no packet loss) so the whole loop is bit-reproducible — the
// lockstep determinism pillar (ADR-GX-005). The real UDP / reliability transport
// (Loopback / HostImpl) is exercised by net_loopback / net_udp_loopback; this
// module proves the replication + prediction + reconciliation LOGIC, which is
// what must stay deterministic across peers.
//
// Determinism: net TUs build -fno-fast-math / -ffp-contract=off; the integrator
// (step_entity) is shared by server, client prediction, and any reference
// integration, so identical inputs reproduce identical state bit-for-bit.

#pragma once

#include "net/Prediction.h"
#include "net/SnapshotReplication.h"
#include "net/TickConfig.h"

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::net {

// Deterministic kinematic step shared by the server advance and client
// prediction: integrate position by the input's move velocity (m/s) over dt and
// copy the look yaw. Pure float ops (no fast-math reassociation).
void step_entity(EntityState& e, const Input& in, f32 dt) noexcept;

// Deterministic ray-vs-entity hitscan: nearest entity (treated as a sphere of
// `radius`) struck by the ray, excluding `exclude_id`. Returns the victim's id
// (0 = miss). Used for lag-compensated server-side hit registration against a
// rewound snapshot. Pure float ops; no trig.
u32 raycast_nearest_entity(std::span<const EntityState> world, u32 exclude_id,
                           const f32 origin[3], const f32 dir[3], f32 radius,
                           f32 max_t) noexcept;

// A server-registered hit (attacker shot victim at server_tick), produced by the
// lag-compensated hitreg path. POD.
struct HitEvent {
    u32 attacker = 0;
    u32 victim = 0;
    u32 server_tick = 0;
};

class ReplicationSession {
public:
    // `client_count` entities (one per client); each snapshot/input crosses the
    // channel after `latency_ticks` ticks each way (>= 0).
    ReplicationSession(const TickConfig& cfg, u32 client_count, u32 latency_ticks);

    // Advance one tick. `inputs` is one Input per client for this tick (its
    // .tick field is stamped with the current tick). Drives: client push +
    // predict + send -> server deliver + apply + snapshot + send -> client
    // deliver + reconcile, all through the latency channel.
    void advance(std::span<const Input> inputs);

    u32 tick() const noexcept { return tick_; }
    u32 client_count() const noexcept { return static_cast<u32>(clients_.size()); }

    // Server-side lag-compensated hits registered so far (attacker fired with
    // kInputBtnFire; the server rewound the world to that client's view-tick
    // before the ray test, so it hits where the shooter SAW the target).
    const std::vector<HitEvent>& hit_events() const noexcept { return hit_events_; }

    // Server-authoritative world (one EntityState per client).
    const std::vector<EntityState>& authoritative() const noexcept { return server_world_; }
    // A client's locally predicted state for its own entity.
    const EntityState& predicted(u32 client) const noexcept { return clients_[client].predicted; }
    // Size in bytes of the most recent per-tick snapshot delta (bandwidth probe;
    // ~header-only when the world is idle, larger when entities move).
    usize last_delta_bytes() const noexcept { return last_delta_bytes_; }

private:
    struct ClientState {
        InputRing<256>           ring;
        EntityState              predicted{};
        std::vector<EntityState> baseline;  // last full snapshot applied (delta base)
        u32                      last_applied_server_tick = 0;
        bool                     has_baseline = false;
    };
    // Lag-compensated hit registration for a fire input applied at `server_tick`
    // from `client`: rewind to the shooter's view-tick and ray-test there.
    void register_fire_(u32 server_tick, u32 client, const Input& in);

    struct C2S {  // client -> server: an input + the client's snapshot ack
        u32   deliver_tick = 0;
        u32   client = 0;
        Input input{};
    };
    struct S2C {  // server -> client: a snapshot delta + the acked input tick
        u32              deliver_tick = 0;
        u32              client = 0;
        u32              server_tick = 0;
        u32              acked_input_tick = 0;
        std::vector<u8>  delta;
    };

    f32 dt_;
    u32 latency_;
    u32 tick_ = 0;

    std::vector<EntityState> server_world_;        // authoritative
    std::vector<EntityState> server_prev_;         // last snapshot sent (delta base)
    std::vector<u32>         server_last_input_;   // per client, last applied input tick
    bool                     server_has_prev_ = false;

    std::vector<ClientState> clients_;
    std::vector<C2S>         c2s_;  // in-flight inputs
    std::vector<S2C>         s2c_;  // in-flight snapshots
    std::vector<Input>       scratch_;  // predict_present pending-input scratch
    usize                    last_delta_bytes_ = 0;

    // Lag-compensation snapshot history: recent authoritative worlds keyed by
    // tick (ring, trimmed to a window > 2*latency). On a fire input the server
    // rewinds to the shooter's view-tick before the ray test, so hits land where
    // the client saw the target — server-authoritative, deterministic.
    std::vector<std::pair<u32, std::vector<EntityState>>> history_;
    std::vector<HitEvent> hit_events_;
};

}  // namespace psynder::net
