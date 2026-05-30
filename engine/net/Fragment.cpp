// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — rUDP packet fragmentation + reassembly. Lane 18 (net).
// See Fragment.h for the wire format + determinism notes.

#include "net/Fragment.h"

namespace psynder::net {

namespace {

// Append a u16 to `out` in little-endian byte order, independent of host
// endianness. We never memcpy the integer (that would be host-order); we shift
// out each byte so the stream is bit-identical on every platform.
inline void put_u16_le(std::vector<u8>& out, u16 v) noexcept {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
}

// Read a little-endian u16 from `p` (caller guarantees 2 readable bytes).
inline u16 read_u16_le(const u8* p) noexcept {
    return static_cast<u16>(static_cast<u16>(p[0]) |
                            (static_cast<u16>(p[1]) << 8));
}

}  // namespace

void fragment_message(u16                           message_id,
                      std::span<const u8>           payload,
                      usize                         max_fragment_bytes,
                      std::vector<std::vector<u8>>& out_fragments) {
    out_fragments.clear();

    // Guard a zero cap: clamp to 1 so we always make progress (1 data byte per
    // fragment) instead of dividing by zero / looping forever.
    if (max_fragment_bytes == 0) max_fragment_bytes = 1;

    const usize payload_size = payload.size();

    // Fragment count = ceil(payload_size / max_fragment_bytes), but always at
    // least 1: an empty payload still produces a single (header-only) fragment.
    usize count = (payload_size + max_fragment_bytes - 1) / max_fragment_bytes;
    if (count == 0) count = 1;

    const u16 fragment_count = static_cast<u16>(count);
    out_fragments.reserve(count);

    for (usize i = 0; i < count; ++i) {
        const usize begin = i * max_fragment_bytes;
        const usize end =
            (begin + max_fragment_bytes < payload_size) ? begin + max_fragment_bytes
                                                         : payload_size;
        const usize slice_len = (end > begin) ? end - begin : 0;

        std::vector<u8> frag;
        frag.reserve(kFragmentHeaderBytes + slice_len);

        put_u16_le(frag, message_id);
        put_u16_le(frag, static_cast<u16>(i));
        put_u16_le(frag, fragment_count);

        if (slice_len > 0) {
            frag.insert(frag.end(), payload.data() + begin, payload.data() + end);
        }

        out_fragments.push_back(std::move(frag));
    }
}

FragmentReassembler::FragmentReassembler() { reset(); }

void FragmentReassembler::reset() noexcept {
    message_id_     = 0;
    fragment_count_ = 0;
    received_       = 0;
    has_message_    = false;
    slices_.clear();
    have_.clear();
}

void FragmentReassembler::begin_message(u16 message_id, u16 fragment_count) {
    message_id_     = message_id;
    fragment_count_ = fragment_count;
    received_       = 0;
    has_message_    = true;
    slices_.assign(fragment_count, std::vector<u8>{});
    have_.assign(fragment_count, false);
}

bool FragmentReassembler::add_fragment(
    std::span<const u8> fragment_with_header) noexcept {
    // Malformed: cannot even hold the 6-byte header.
    if (fragment_with_header.size() < kFragmentHeaderBytes) return false;

    const u8* p = fragment_with_header.data();
    const u16 message_id     = read_u16_le(p + 0);
    const u16 fragment_index = read_u16_le(p + 2);
    const u16 fragment_count = read_u16_le(p + 4);

    // Malformed: a real message has at least one fragment, and the index must
    // fall inside the declared count.
    if (fragment_count == 0) return false;
    if (fragment_index >= fragment_count) return false;

    // A fragment for a NEW message id starts a fresh reassembly, discarding any
    // partial old message. A first-ever fragment likewise begins a message.
    if (!has_message_ || message_id != message_id_) {
        begin_message(message_id, fragment_count);
    } else if (fragment_count != fragment_count_) {
        // Same id but an inconsistent count — treat as a fresh (re)start rather
        // than indexing a stale-sized buffer.
        begin_message(message_id, fragment_count);
    }

    // Duplicate slice for the current message: ignore (still counts as having
    // arrived once, so completion is not double-counted).
    if (have_[fragment_index]) return false;

    const usize data_len = fragment_with_header.size() - kFragmentHeaderBytes;
    std::vector<u8>& dst  = slices_[fragment_index];
    dst.assign(p + kFragmentHeaderBytes, p + kFragmentHeaderBytes + data_len);

    have_[fragment_index] = true;
    ++received_;
    return true;
}

bool FragmentReassembler::complete() const noexcept {
    return has_message_ && fragment_count_ > 0 && received_ == fragment_count_;
}

bool FragmentReassembler::assemble(std::vector<u8>& out) const noexcept {
    if (!complete()) return false;

    out.clear();
    usize total = 0;
    for (const std::vector<u8>& s : slices_) total += s.size();
    out.reserve(total);

    for (const std::vector<u8>& s : slices_) {
        out.insert(out.end(), s.begin(), s.end());
    }
    return true;
}

}  // namespace psynder::net
