// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — end-to-end snapshot replication pipeline. See
// ReplicationPipeline.h for the full server->client data-flow contract.
//
// This TU is pure composition: it drives the already-tested net primitives in
// the order a real server/client pair runs them. No new mechanism, no floats of
// its own beyond passing the configured resolution / dt through to the
// underlying deterministic stages. Strict-FP net lane
// (-fno-fast-math -ffp-contract=off, DESIGN-PSYNDER-GX.md §14).

#include "net/ReplicationPipeline.h"

#include "net/SnapshotPack.h"  // unpack_quantized

namespace psynder::net {

// ── ReplicationServer ──────────────────────────────────────────────────────

void ReplicationServer::configure(usize entity_count, f32 bytes_per_s,
                                  usize burst, f32 pos_res, usize mtu) {
    scheduler_.configure(entity_count);
    budget_.configure(bytes_per_s, burst);
    budget_.reset();  // start each (re)configure with an empty bucket
    pos_res_ = pos_res;
    mtu_     = mtu;

    plan_.sent_indices.clear();
    plan_.record_count   = 0;
    plan_.payload_bytes  = 0;
    plan_.fragment_count = 0;
    plan_.budget_spent   = 0;
}

void ReplicationServer::reset() noexcept {
    scheduler_.reset();
    budget_.reset();
}

void ReplicationServer::tick(std::span<const EntityState> states,
                             std::span<const f32>         base_priorities,
                             f32                          dt_s,
                             u16                          msg_id,
                             std::vector<std::vector<u8>>& out_fragments) {
    // 1. Pour this frame's bytes into the send budget (capped at the burst).
    budget_.refill(dt_s);

    // 2. Schedule + pack + fragment the highest-priority entities the budget
    //    can afford. The scheduler clears out_fragments and fills plan_.
    scheduler_.tick(states, base_priorities, budget_, pos_res_, msg_id, mtu_,
                    out_fragments, plan_);
}

// ── ReplicationClient ──────────────────────────────────────────────────────

void ReplicationClient::configure(f32 pos_res, f32 interp_delay_s) {
    pos_res_ = pos_res;
    clock_.configure(interp_delay_s);
    reset();
}

void ReplicationClient::reset() noexcept {
    reassembler_.reset();
    buffer_.clear();
    clock_.reset();
}

bool ReplicationClient::receive(std::span<const std::vector<u8>> fragments,
                                f64 server_time_s) {
    // 1. Reassemble this tick's fragments (tolerant of arrival order / dupes).
    //    A fresh reassembler per tick keeps each snapshot's reassembly isolated
    //    from any half-delivered neighbour.
    reassembler_.reset();
    for (const std::vector<u8>& f : fragments) {
        reassembler_.add_fragment(f);
    }
    if (!reassembler_.complete()) {
        return false;  // incomplete fragment set — drop the whole snapshot
    }
    if (!reassembler_.assemble(payload_scratch_)) {
        return false;
    }

    // 2. Dequantize the packed payload back to EntityStates.
    if (!unpack_quantized(payload_scratch_, pos_res_, states_scratch_)) {
        return false;  // malformed / truncated payload
    }

    // 3. Stamp the recovered states into the interp ring at the server time.
    buffer_.push(server_time_s, states_scratch_);

    // 4. Record the server time as the newest data the render clock trails.
    clock_.on_snapshot(server_time_s);
    return true;
}

void ReplicationClient::advance(f32 dt_s) {
    // 5. Move the local render clock forward and re-sync it toward the target
    //    (newest_server_time - interp_delay).
    clock_.advance(dt_s);
}

bool ReplicationClient::view(std::vector<EntityState>& out) const {
    // 6. Sample the interp buffer at the playout render time: LERP every entity
    //    between the two snapshots that bracket the render time.
    return buffer_.sample(clock_.render_time_s(), out);
}

}  // namespace psynder::net
