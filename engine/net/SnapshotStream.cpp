// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — stateful baseline-tracking delta snapshot stream. Lane 18 (net).
// See SnapshotStream.h for the dequantized-baseline invariant + determinism.

#include "net/SnapshotStream.h"

#include "net/SnapshotPackDelta.h"
#include "net/SnapshotQuantized.h"

#include <algorithm>

namespace psynder::net {

void SnapshotStreamSender::encode(std::span<const EntityState> curr,
                                  f32                          pos_resolution_m,
                                  std::vector<u8>&             out) {
    // 1) Emit the delta from the CURRENT baseline to `curr`. The codec clears
    //    `out` and writes the removed-id list + changed records (ascending id).
    pack_quantized_delta(baseline_, curr, pos_resolution_m, out);

    // 2) Advance the baseline to exactly what the receiver will reconstruct from
    //    this delta: the DEQUANTIZED round-trip of `curr`. Round-tripping each
    //    state through quantize_state -> dequantize_state collapses any sub-step
    //    float jitter to the same lattice point the receiver lands on, so the two
    //    baselines stay bit-identical even for entities omitted from the delta
    //    (a float move < one quantization step). We do NOT keep the raw floats.
    //
    //    Order matters: the codec's apply emits its output ascending by id, so we
    //    sort the advanced baseline ascending here to match the receiver exactly.
    next_.clear();
    next_.reserve(curr.size());
    for (const EntityState& s : curr) {
        next_.push_back(
            dequantize_state(quantize_state(s, pos_resolution_m), pos_resolution_m));
    }
    std::sort(next_.begin(), next_.end(),
              [](const EntityState& a, const EntityState& b) { return a.id < b.id; });

    // Swap the freshly-built baseline into place (no extra allocation; the old
    // baseline storage is recycled as next tick's scratch).
    baseline_.swap(next_);
}

void SnapshotStreamSender::reset() noexcept {
    baseline_.clear();
    next_.clear();
}

bool SnapshotStreamReceiver::decode(std::span<const u8>       bytes,
                                    f32                       pos_resolution_m,
                                    std::vector<EntityState>& out) {
    // apply_quantized_delta validates the buffer fully before touching `out`, so
    // on a malformed/truncated delta it returns false and leaves `out` intact.
    if (!apply_quantized_delta(baseline_, bytes, pos_resolution_m, out)) {
        return false;  // baseline_ and out both untouched — drop the bad packet
    }

    // Success: advance the tracked baseline to the reconstructed set so the next
    // delta diffs against the same state the sender now holds. `out` is already
    // ascending by id (the codec emits it that way), matching the sender's
    // sorted advance — the two baselines are bit-identical.
    baseline_ = out;
    return true;
}

void SnapshotStreamReceiver::reset() noexcept {
    baseline_.clear();
}

}  // namespace psynder::net
