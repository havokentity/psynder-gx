// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — stateful baseline-tracking delta snapshot stream. Lane 18 (net).
//
// SnapshotPackDelta.h is a STATELESS codec: every call diffs an explicit `curr`
// against an explicit `prev` baseline the caller must supply. A real wire path
// does not carry a fresh baseline each tick — it sends a delta against the LAST
// snapshot, and BOTH ends must independently track the SAME evolving baseline or
// successive deltas drift apart. This header is that stateful streaming layer:
// a `SnapshotStreamSender` and a `SnapshotStreamReceiver`, each holding the
// baseline they share, wrapped around the pure pack/apply codec.
//
// THE CRUCIAL INVARIANT — the sender tracks the DEQUANTIZED baseline:
//   The receiver only ever observes DEQUANTIZED states (it reconstructs each
//   tick from the delta + its baseline, then dequantizes). So the sender's
//   tracked baseline must equal what the receiver reconstructs — the dequantized
//   round-trip of the snapshot, NOT the raw input floats. If the sender instead
//   diffed against the raw floats, an entity whose float moved LESS than one
//   quantization step (so it is correctly OMITTED from the delta) would still
//   differ between the sender's float baseline and the receiver's dequantized
//   baseline; the omission would then silently desync the two sides forever.
//   By advancing BOTH baselines to the dequantized round-trip
//   (dequantize(quantize(curr))), sender and receiver stay BIT-IDENTICAL tick
//   after tick. See SnapshotQuantized.h for the round-trip bounds.
//
// Determinism: strict-FP net lane (-fno-fast-math -ffp-contract=off,
// DESIGN-PSYNDER-GX.md §14). All logic delegates to the pure quantize/dequantize
// stages and the integer delta codec — no transcendentals, no RNG. Ascending-id
// ordering is inherited from the codec (apply emits id-sorted), and the sender's
// baseline advance re-sorts to match. Steady state performs no per-call heap
// growth beyond the caller-owned `out` (member scratch vectors are reused).

#pragma once

#include "core/Types.h"
#include "net/SnapshotReplication.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// SnapshotStreamSender — the sending half of the stateful stream. Holds the
// baseline the receiver is known to hold and, on each `encode`, emits a delta
// from that baseline to `curr` then advances the baseline to the DEQUANTIZED
// round-trip of `curr` (what the receiver will reconstruct). Reuses member
// scratch so the steady-state encode allocates nothing beyond the caller's
// `out`.
// ──────────────────────────────────────────────────────────────────────────
class SnapshotStreamSender {
public:
    // Append a delta of `curr` against the tracked baseline to `out` (cleared by
    // the codec), then advance the baseline to dequantize(quantize(curr)) — the
    // exact set the receiver reconstructs from this same delta. `out` carries the
    // wire bytes; the baseline mutation is internal.
    void encode(std::span<const EntityState> curr,
                f32                          pos_resolution_m,
                std::vector<u8>&             out);

    // Clear the tracked baseline. The next `encode` diffs against the empty set,
    // i.e. produces a full add-everything delta (a keyframe).
    void reset() noexcept;

    // The tracked baseline — the dequantized snapshot the receiver should hold
    // after the last encoded delta. Exposed for tests / introspection.
    const std::vector<EntityState>& baseline() const noexcept { return baseline_; }

private:
    std::vector<EntityState> baseline_;  // dequantized round-trip of the last curr
    std::vector<EntityState> next_;      // scratch: the advanced baseline, reused
};

// ──────────────────────────────────────────────────────────────────────────
// SnapshotStreamReceiver — the receiving half. Holds the same evolving baseline
// and, on each `decode`, applies the delta to it. On success it advances the
// baseline to the reconstructed set (so it matches the sender) and returns true;
// on a malformed/truncated buffer it leaves both the baseline AND `out`
// untouched and returns false.
// ──────────────────────────────────────────────────────────────────────────
class SnapshotStreamReceiver {
public:
    // Apply `bytes` (a delta from the tracked baseline) into `out`. On success
    // `out` holds the reconstructed dequantized set, the baseline is advanced to
    // a copy of it, and the call returns true. On failure (the codec rejects the
    // bytes) the baseline and `out` are both left intact and the call returns
    // false — a dropped/corrupt packet never corrupts the tracked baseline.
    bool decode(std::span<const u8>       bytes,
                f32                       pos_resolution_m,
                std::vector<EntityState>& out);

    // Clear the tracked baseline. The next `decode` must carry a full keyframe
    // (matching a sender `reset`) to re-establish a shared baseline.
    void reset() noexcept;

    // The tracked baseline — the last successfully reconstructed set. After every
    // matched tick this equals the sender's baseline() bit-for-bit.
    const std::vector<EntityState>& baseline() const noexcept { return baseline_; }

private:
    std::vector<EntityState> baseline_;  // last reconstructed set (= sender side)
};

}  // namespace psynder::net
