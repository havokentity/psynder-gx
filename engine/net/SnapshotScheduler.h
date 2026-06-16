// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — bandwidth-managed snapshot send pipeline. Lane 18 (net).
//
// This is the INTEGRATION that ties the net lane's scheduling primitives into
// the real answer to "what does the server actually send each peer this tick?":
//
//   PriorityAccumulator  — of the relevant entities, which want to be sent most
//                          (and which climb the queue when they miss a tick),
//   BandwidthBudget      — a per-tick byte budget (a token bucket = send-rate *
//                          MTU) capping how many records fit,
//   pack_quantized       — the chosen entities' float states quantized + packed
//                          to a compact, cross-platform-bitwise byte payload,
//   fragment_message     — the payload split to the link MTU for the wire.
//
// Each tick: accumulate priorities, take the highest-priority entities that the
// budget can afford (one quantized record = `quantized_wire_bytes()`), reset
// those accumulators (so the losers climb — no starvation), pack their quantized
// states, and fragment the packet. A peer reassembles the fragments
// (FragmentReassembler) and unpacks (unpack_quantized) to recover exactly the
// records that were scheduled this tick.
//
// SCOPE: a scheduler over a SNAPSHOT of authoritative state — it never feeds back
// into the lockstep sim, so it cannot change the deterministic game outcome. It
// still lives in the strict-FP net lane, so for a given op sequence the selection,
// the packed bytes, and the fragments are bit-reproducible across runs/platforms.

#pragma once

#include "net/BandwidthBudget.h"
#include "net/PriorityAccumulator.h"
#include "net/SnapshotReplication.h"  // EntityState

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::net {

class SnapshotScheduler {
public:
    // What a single tick scheduled — for tests / metrics / the caller's bookkeeping.
    struct SendPlan {
        std::vector<u32> sent_indices;       // entity indices sent (descending priority)
        usize            record_count = 0;   // == sent_indices.size()
        usize            payload_bytes = 0;  // packed snapshot size before fragmenting
        usize            fragment_count = 0; // number of MTU-sized fragments produced
        usize            budget_spent = 0;   // bytes charged to the budget this tick
    };

    SnapshotScheduler() noexcept = default;

    // Size the per-entity priority space (dense indices [0, entity_count)); the
    // caller maps its entity ids onto these slots, parallel to `states`.
    void configure(usize entity_count);

    // Zero every accumulator (e.g. on a peer (re)connect).
    void reset() noexcept;

    usize entity_count() const noexcept { return prio_.size(); }

    // One send tick. `states[i]` is entity slot i's authoritative state and
    // `base_priorities[i]` how badly it wants to be sent (higher = more often).
    // Accumulate, pick the top entities the `budget` can afford (one record =
    // quantized_wire_bytes()), charge the budget, quantize+pack them, and fragment
    // to `mtu_bytes`. `message_id` stamps the fragment set. On an empty selection
    // (no budget or no positive priority) `out_fragments` is cleared and the plan
    // is all-zero. `pos_resolution_m` is the position quantization step.
    void tick(std::span<const EntityState> states,
              std::span<const f32>         base_priorities,
              BandwidthBudget&             budget,
              f32                          pos_resolution_m,
              u16                          message_id,
              usize                        mtu_bytes,
              std::vector<std::vector<u8>>& out_fragments,
              SendPlan&                     out_plan);

    // Read access to the underlying accumulator (for tests / introspection).
    const PriorityAccumulator& priorities() const noexcept { return prio_; }

private:
    PriorityAccumulator      prio_;
    std::vector<EntityState> sent_scratch_;    // reused: the selected subset
    std::vector<u8>          packet_scratch_;  // reused: the packed payload
};

}  // namespace psynder::net
