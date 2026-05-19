// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — bridge from lane 17's OpaqueWorldState producer to lane 18's
// LagComp rewind. Header-only so lane 18 can pick it up without growing its
// own translation unit; the symbol resolves through `psynder_worldstate`.
//
// Lane 18's `rewind_world_to` is currently the byte-lerp fallback that picks
// the nearer-bracket snapshot because the net layer doesn't own the wire
// format. When lane 18 wants to do the proper sub-tick lerp it calls
// `psynder::worldstate::lerp_for_lagcomp(a, sa, b, sb, t, out, so)`.
//
// IMPORTANT: this is the producer-side wiring only. The matching call site
// inside `LagComp.cpp` (or the rolling-buffer code in M6) lands in a
// separate PR against `lane/18-net` — the contract here is "the symbol
// exists and the signature matches". Crossing the wire from lane 17 to
// lane 18 in one PR would deadlock the merge order.

#pragma once

#include "WorldState.h"

#include <cstddef>
#include <span>

namespace psynder::worldstate {

// Sized for lane 18's OpaqueWorldState.size field (std::size_t bytes).
//
// Returns true on success and writes `out_size` bytes of interpolated state
// into `out_bytes`.  Returns false on:
//   * mismatched buffer sizes between `a_bytes`, `b_bytes`, `out_bytes`
//   * either input failing header validation
//   * entity layouts that don't match (id or archetype mismatch)
//
// On false, `out_bytes` is left UNDISTURBED — the header write is deferred
// until after all validation passes, so a failed call can be retried with
// a different b/t without first wiping `out`.  Lane 18 should fall back to
// its nearest-bracket pick on false.
inline bool lerp_for_lagcomp(const void* a_bytes, std::size_t a_size,
                             const void* b_bytes, std::size_t b_size,
                             float       t,
                             void*       out_bytes, std::size_t out_size) noexcept {
    if (!a_bytes || !b_bytes || !out_bytes) return false;
    if (a_size == 0 || b_size == 0 || out_size == 0) return false;
    // The byte buffers MUST be the same length: the wire format is fully
    // defined by (header.entity_count) and any mismatch means lane 18 fed us
    // snapshots from different rosters, which we can't blend by construction.
    if (a_size != b_size || a_size != out_size) return false;

    return interpolate_world_state(
        std::span<const u8>(static_cast<const u8*>(a_bytes), a_size),
        std::span<const u8>(static_cast<const u8*>(b_bytes), b_size),
        static_cast<f32>(t),
        std::span<u8>(static_cast<u8*>(out_bytes), out_size));
}

}  // namespace psynder::worldstate
