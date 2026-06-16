// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — quantized snapshot DELTA codec. Lane 18 (net).
// See SnapshotPackDelta.h for the wire format + determinism notes.

#include "net/SnapshotPackDelta.h"

#include "net/SnapshotQuantized.h"

#include <algorithm>
#include <cstring>

namespace psynder::net {

namespace {

// On-wire layout constants (all integers, explicit little-endian). A delta is
// two length-prefixed lists: the removed-id list (u32 count + count * u32 id)
// and the changed-record list (u32 count + count * 20-byte record).
constexpr usize kCountBytes  = sizeof(u32);                  // a u32 list count
constexpr usize kIdBytes     = sizeof(u32);                  // a removed id
constexpr usize kRecordBytes =
    sizeof(u32) + 3 * sizeof(i32) + sizeof(i32);             // id + pos[3] + yaw

// Append a u32 to `out` in little-endian byte order, independent of host
// endianness. We never memcpy the integer (that would be host-order); we shift
// out each byte so the stream is bit-identical on every platform. Mirrors the
// put_u32_le helper in SnapshotPack.cpp exactly.
inline void put_u32_le(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

// Append an i32 as little-endian. Reinterpret the bit pattern as u32 first so
// the byte layout is well-defined for negative values (two's-complement bits
// preserved) without relying on signed-shift behaviour. Mirrors put_i32_le.
inline void put_i32_le(std::vector<u8>& out, i32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32_le(out, bits);
}

// Read a little-endian u32 from `p` (caller guarantees 4 readable bytes).
inline u32 read_u32_le(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

// Read a little-endian i32 from `p` (caller guarantees 4 readable bytes).
inline i32 read_i32_le(const u8* p) noexcept {
    const u32 bits = read_u32_le(p);
    i32 v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// Bit-exact equality of two quantized records (compare the integer fields, not
// the floats). Two entities that quantize to the same record are "unchanged"
// and dropped from the delta — lockstep determinism forbids float epsilon fuzz.
inline bool quantized_equal(const QuantizedState& a, const QuantizedState& b) noexcept {
    return a.id == b.id &&
           a.pos[0] == b.pos[0] &&
           a.pos[1] == b.pos[1] &&
           a.pos[2] == b.pos[2] &&
           a.yaw_milli == b.yaw_milli;
}

// Quantize a state set into `scratch`, then sort it ascending by id so both
// directions (pack + apply) walk a canonical, input-order-independent ordering.
// `scratch` is cleared/reserved — no exotic per-call allocation.
void quantize_sorted(std::span<const EntityState> states,
                     f32                          pos_resolution_m,
                     std::vector<QuantizedState>& scratch) {
    scratch.clear();
    scratch.reserve(states.size());
    for (const EntityState& s : states) {
        scratch.push_back(quantize_state(s, pos_resolution_m));
    }
    std::sort(scratch.begin(), scratch.end(),
              [](const QuantizedState& a, const QuantizedState& b) {
                  return a.id < b.id;
              });
}

}  // namespace

void pack_quantized_delta(std::span<const EntityState> prev,
                          std::span<const EntityState> curr,
                          f32                          pos_resolution_m,
                          std::vector<u8>&             out) {
    out.clear();

    // Quantize + id-sort both sides into local scratch. Sorted order lets us
    // walk prev and curr with a single merge, and makes the byte stream depend
    // only on the entity set, not on the caller's input ordering.
    std::vector<QuantizedState> qp;
    std::vector<QuantizedState> qc;
    quantize_sorted(prev, pos_resolution_m, qp);
    quantize_sorted(curr, pos_resolution_m, qc);

    // ── Pass 1: removed ids — present in prev (qp) but absent from curr (qc),
    // gathered in ascending id order by the merge walk. ──────────────────────
    std::vector<u32> removed;
    removed.reserve(qp.size());
    {
        usize i = 0;  // index into qp
        usize j = 0;  // index into qc
        while (i < qp.size()) {
            // Advance curr past any ids smaller than the current prev id.
            while (j < qc.size() && qc[j].id < qp[i].id) ++j;
            if (j < qc.size() && qc[j].id == qp[i].id) {
                // Present in both — not removed.
                ++i;
            } else {
                // prev id has no curr match — removed.
                removed.push_back(qp[i].id);
                ++i;
            }
        }
    }

    // ── Pass 2: changed records — each curr entity that is NEW (id absent from
    // prev) OR whose quantized record differs from its prev counterpart, in
    // ascending id order (qc is already id-sorted). ──────────────────────────
    put_u32_le(out, static_cast<u32>(removed.size()));
    for (const u32 id : removed) {
        put_u32_le(out, id);
    }

    // Back-patch the changed count once known; reserve its slot first so the
    // record bytes can stream in directly after it.
    const usize changed_count_at = out.size();
    put_u32_le(out, 0);

    u32 changed = 0;
    {
        usize i = 0;  // index into qp
        for (const QuantizedState& c : qc) {
            // Advance prev past any ids smaller than this curr id.
            while (i < qp.size() && qp[i].id < c.id) ++i;
            const bool has_prev = (i < qp.size() && qp[i].id == c.id);
            if (has_prev && quantized_equal(qp[i], c)) {
                continue;  // unchanged — omit from the delta
            }
            put_u32_le(out, c.id);
            put_i32_le(out, c.pos[0]);
            put_i32_le(out, c.pos[1]);
            put_i32_le(out, c.pos[2]);
            put_i32_le(out, c.yaw_milli);
            ++changed;
        }
    }

    // Patch the changed count in place, byte-by-byte in LE order (same shift
    // convention as put_u32_le — never a struct memcpy of the integer).
    out[changed_count_at + 0] = static_cast<u8>(changed & 0xFFu);
    out[changed_count_at + 1] = static_cast<u8>((changed >> 8) & 0xFFu);
    out[changed_count_at + 2] = static_cast<u8>((changed >> 16) & 0xFFu);
    out[changed_count_at + 3] = static_cast<u8>((changed >> 24) & 0xFFu);
}

bool apply_quantized_delta(std::span<const EntityState> prev,
                           std::span<const u8>          bytes,
                           f32                          pos_resolution_m,
                           std::vector<EntityState>&    out) {
    // ── Parse + validate the removed list. ───────────────────────────────────
    if (bytes.size() < kCountBytes) return false;  // removed-count header missing

    usize off = 0;
    const u32 removed_count = read_u32_le(bytes.data() + off);
    off += kCountBytes;

    // The removed ids must fit, plus the changed-count header that follows them.
    const usize after_removed =
        off + static_cast<usize>(removed_count) * kIdBytes;
    if (bytes.size() < after_removed + kCountBytes) return false;  // truncated

    // ── Parse + validate the changed list. ───────────────────────────────────
    usize changed_off = after_removed;
    const u32 changed_count = read_u32_le(bytes.data() + changed_off);
    changed_off += kCountBytes;

    const usize need =
        changed_off + static_cast<usize>(changed_count) * kRecordBytes;
    if (bytes.size() < need) return false;  // declared records run past the end

    // Buffer is fully validated; from here we never touch `out` until we are
    // certain we will succeed — so a malformed buffer leaves `out` untouched.

    // Quantize + id-sort the baseline; this is the starting point we mutate.
    std::vector<QuantizedState> recs;
    quantize_sorted(prev, pos_resolution_m, recs);

    // Drop the removed ids. Each removed id is read LE from the buffer; we
    // remove the matching baseline record (linear scan — the scaffold keeps
    // baselines small, matching SnapshotReplication's find_state TODO note).
    for (u32 r = 0; r < removed_count; ++r) {
        const u32 id = read_u32_le(bytes.data() + off);
        off += kIdBytes;
        for (usize k = 0; k < recs.size(); ++k) {
            if (recs[k].id == id) {
                recs.erase(recs.begin() + static_cast<std::ptrdiff_t>(k));
                break;
            }
        }
    }

    // Overlay each changed record: replace a matching id, else insert it. We
    // keep `recs` id-sorted so the final emit is ascending without a re-sort.
    for (u32 c = 0; c < changed_count; ++c) {
        QuantizedState q{};
        q.id        = read_u32_le(bytes.data() + changed_off); changed_off += sizeof(u32);
        q.pos[0]    = read_i32_le(bytes.data() + changed_off); changed_off += sizeof(i32);
        q.pos[1]    = read_i32_le(bytes.data() + changed_off); changed_off += sizeof(i32);
        q.pos[2]    = read_i32_le(bytes.data() + changed_off); changed_off += sizeof(i32);
        q.yaw_milli = read_i32_le(bytes.data() + changed_off); changed_off += sizeof(i32);

        // Lower-bound insertion point by id keeps the vector sorted ascending.
        const auto it = std::lower_bound(
            recs.begin(), recs.end(), q.id,
            [](const QuantizedState& s, u32 id) { return s.id < id; });
        if (it != recs.end() && it->id == q.id) {
            *it = q;  // replace existing record
        } else {
            recs.insert(it, q);  // new id — insert in order
        }
    }

    // All survivors are now id-sorted; dequantize each into the output.
    out.clear();
    out.reserve(recs.size());
    for (const QuantizedState& q : recs) {
        out.push_back(dequantize_state(q, pos_resolution_m));
    }
    return true;
}

usize delta_size(usize removed_count, usize changed_count) noexcept {
    // 4 (removed count) + removed_count*4 + 4 (changed count) + changed_count*20.
    return kCountBytes + removed_count * kIdBytes +
           kCountBytes + changed_count * kRecordBytes;
}

}  // namespace psynder::net
