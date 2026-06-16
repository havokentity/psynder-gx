// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — rUDP packet fragmentation + reassembly. Lane 18 (net).
//
// A datagram transport has a hard per-packet ceiling (the path MTU). Any
// reliable message larger than that ceiling must be split into numbered
// fragments, shipped independently, and stitched back together on the far end.
// This is the rUDP large-message primitive: `fragment_message` cuts a payload
// into MTU-sized pieces, each carrying a tiny header; `FragmentReassembler`
// collects those pieces back into the original byte-exact payload, tolerating
// out-of-order arrival, duplicates, and a sender that abandons one message
// mid-stream and starts another.
//
// Wire format (FragmentHeader, explicit little-endian, 6 bytes, prepended to
// every fragment's data slice):
//   offset 0..1 : u16 message_id      — identifies the logical message
//   offset 2..3 : u16 fragment_index  — 0-based position of this slice
//   offset 4..5 : u16 fragment_count  — total fragments in the message
//   offset 6..  : the fragment's slice of the original payload
// Each integer is emitted byte-by-byte low-byte-first; we never memcpy the
// FragmentHeader struct across the wire, so the bytes are identical on a
// big-endian and a little-endian machine (the struct's in-memory padding /
// host order never leak into the stream). The receiver parses the six header
// bytes back with pure integer shifts.
//
// Determinism / safety: strict-FP net lane (-fno-fast-math -ffp-contract=off,
// DESIGN-PSYNDER-GX.md §14). Every operation here is pure integer byte
// shuffling — no floats, no transcendentals, no RNG. `fragment_message` then
// reassemble round-trips ANY payload exactly. The only heap is the explicit
// output containers (the fragment vectors, the assembled payload); parsing a
// fragment never allocates.

#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// FragmentHeader — the 6-byte little-endian prefix on every fragment. This is
// the logical/in-memory view; the on-wire encoding is the explicit LE byte
// layout documented in the header banner (NOT a memcpy of this struct).
// ──────────────────────────────────────────────────────────────────────────
struct FragmentHeader {
    u16 message_id     = 0;  // identifies the logical message
    u16 fragment_index = 0;  // 0-based position of this slice
    u16 fragment_count = 0;  // total number of fragments in the message
};

// Size of the on-wire header prefix, in bytes (u16 * 3 = 6).
inline constexpr usize kFragmentHeaderBytes = 3 * sizeof(u16);

// ──────────────────────────────────────────────────────────────────────────
// fragment_message — split `payload` into ceil(payload.size /
// max_fragment_bytes) fragments (always >= 1, so an EMPTY payload still yields
// exactly one fragment). Each output fragment is the 6-byte LE header
// (message_id, fragment_index, fragment_count) followed by that fragment's
// slice of the payload. `out_fragments` is cleared and then filled in index
// order, giving a stable, reproducible byte stream.
//
// `max_fragment_bytes` is the cap on the DATA carried per fragment (the header
// is additional). A value of 0 is a contract violation and is clamped to 1 so
// the function always makes progress instead of looping forever.
// ──────────────────────────────────────────────────────────────────────────
void fragment_message(u16                             message_id,
                      std::span<const u8>             payload,
                      usize                           max_fragment_bytes,
                      std::vector<std::vector<u8>>&   out_fragments);

// ──────────────────────────────────────────────────────────────────────────
// FragmentReassembler — collects the fragments of ONE message at a time back
// into the original payload. Fragments may arrive out of order and may be
// duplicated; both are handled. The reassembler tracks a single "current"
// message id: when a fragment for a NEW id arrives it discards any partially
// collected old message and starts fresh, so a sender that abandons a message
// mid-stream never corrupts the next one.
// ──────────────────────────────────────────────────────────────────────────
class FragmentReassembler {
public:
    explicit FragmentReassembler();

    // Drop all state — current message id, stored slices, received count.
    void reset() noexcept;

    // Parse + ingest one fragment (header + data). Returns true if the fragment
    // was accepted and stored, false if it was ignored. A fragment is ignored
    // when it is malformed (fewer than 6 bytes, a zero fragment_count, or an
    // index >= count), or when it duplicates a slice already held for the
    // current message. A fragment whose message_id differs from the current one
    // STARTS a new message (the old partial is discarded) and is then stored.
    bool add_fragment(std::span<const u8> fragment_with_header) noexcept;

    // True once every slice [0, fragment_count) of the current message has been
    // received. False before any fragment has arrived.
    bool complete() const noexcept;

    // If complete(), concatenate the stored slices in index order into `out`
    // (cleared first) and return true. Otherwise leave `out` untouched and
    // return false.
    bool assemble(std::vector<u8>& out) const noexcept;

    // The message id currently being reassembled (meaningful only once at least
    // one fragment has been accepted).
    u16 current_message_id() const noexcept { return message_id_; }

    // Number of distinct fragments stored for the current message so far.
    u16 received_count() const noexcept { return received_; }

private:
    void begin_message(u16 message_id, u16 fragment_count);

    u16                          message_id_     = 0;
    u16                          fragment_count_ = 0;
    u16                          received_       = 0;
    bool                         has_message_    = false;
    std::vector<std::vector<u8>> slices_;   // indexed by fragment_index
    std::vector<bool>            have_;     // received-bitmask, per index
};

}  // namespace psynder::net
