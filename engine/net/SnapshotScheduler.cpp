// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — bandwidth-managed snapshot send pipeline. See SnapshotScheduler.h.

#include "net/SnapshotScheduler.h"

#include "net/Fragment.h"
#include "net/SnapshotPack.h"
#include "net/SnapshotQuantized.h"  // quantized_wire_bytes

namespace psynder::net {

void SnapshotScheduler::configure(usize entity_count) {
    prio_.resize(entity_count);
}

void SnapshotScheduler::reset() noexcept { prio_.reset(); }

void SnapshotScheduler::tick(std::span<const EntityState> states,
                             std::span<const f32>         base_priorities,
                             BandwidthBudget&             budget,
                             f32                          pos_resolution_m,
                             u16                          message_id,
                             usize                        mtu_bytes,
                             std::vector<std::vector<u8>>& out_fragments,
                             SendPlan&                     out_plan) {
    out_fragments.clear();
    out_plan.sent_indices.clear();
    out_plan.record_count = 0;
    out_plan.payload_bytes = 0;
    out_plan.fragment_count = 0;
    out_plan.budget_spent = 0;

    // Every entity that wants sending climbs the queue this tick.
    prio_.accumulate(base_priorities);

    // How many fixed-size quantized records can the budget afford right now? One
    // record is quantized_wire_bytes(); guard a degenerate record size. Tolerate
    // sub-byte float-refill undershoot (a "60-byte" refill can land at 59.9997)
    // with a small epsilon so the budget affords the records it nominally should.
    const usize record_bytes = quantized_wire_bytes();
    if (record_bytes == 0) {
        return;
    }
    const f32 avail = budget.available() + 0.001f;
    const usize affordable =
        static_cast<usize>(avail / static_cast<f32>(record_bytes));
    if (affordable == 0) {
        return;  // no budget this tick
    }

    // Take the highest-priority entities that fit (select skips acc <= 0 and
    // resets the winners, so the losers keep climbing — no starvation).
    prio_.select(affordable, out_plan.sent_indices);
    const usize sent = out_plan.sent_indices.size();
    if (sent == 0) {
        return;  // nothing has positive priority
    }

    // Charge the budget for exactly what we send (fixed per-record cost).
    const usize spend = sent * record_bytes;
    budget.try_spend(spend);
    out_plan.budget_spent = spend;

    // Gather the selected entities' states (in the selection's priority order)
    // and quantize-pack them into the payload.
    sent_scratch_.clear();
    sent_scratch_.reserve(sent);
    for (const u32 idx : out_plan.sent_indices) {
        if (idx < states.size()) {
            sent_scratch_.push_back(states[idx]);
        }
    }
    pack_quantized(sent_scratch_, pos_resolution_m, packet_scratch_);
    out_plan.payload_bytes = packet_scratch_.size();

    // Split the packed payload to the link MTU.
    fragment_message(message_id, packet_scratch_, mtu_bytes, out_fragments);
    out_plan.record_count = sent_scratch_.size();
    out_plan.fragment_count = out_fragments.size();
}

}  // namespace psynder::net
