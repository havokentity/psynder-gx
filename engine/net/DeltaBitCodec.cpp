// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — BIT-LEVEL quantized snapshot DELTA codec. Lane 18 (net).
// See DeltaBitCodec.h for the wire format + determinism notes.

#include "net/DeltaBitCodec.h"

#include "net/BitPacker.h"
#include "net/SnapshotQuantized.h"

#include <algorithm>

namespace psynder::net {

namespace {

// Fixed field widths (in bits). Counts and ids are whole u32s on the wire; a
// per-field delta is a 6-bit width header (a value in [0, 64], so 6 bits hold
// any bits_needed result) followed by that many bits of zigzag payload.
constexpr u32 kCountBits = 32;
constexpr u32 kIdBits    = 32;
constexpr u32 kWidthBits = 6;

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

// Quantize a state set into `scratch`, then sort ascending by id so both
// directions (encode + decode) walk a canonical, input-order-independent
// ordering. `scratch` is cleared/reserved — no exotic per-call allocation.
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

// Write one field delta: zigzag(curr - prev) as a 6-bit width header + that many
// payload bits. A zero delta zigzags to 0, bits_needed(0) == 0, so the field is
// just a 6-bit zero width and no payload — the steady-state win.
inline void write_field(BitWriter& w, i32 curr, i32 prev) {
    const u64 zz    = zigzag_encode(static_cast<i64>(curr) - static_cast<i64>(prev));
    const u32 width = bits_needed(zz);
    w.write_bits(width, kWidthBits);
    w.write_bits(zz, width);
}

// Read one field delta written by write_field and apply it onto a baseline.
// Reads the 6-bit width then that many payload bits, zigzag-decodes, and adds the
// signed delta to `prev`. On a truncated buffer the BitReader returns zero-padded
// bits and trips ok(); the caller checks ok() once after the whole parse.
inline i32 read_field(BitReader& r, i32 prev) {
    const u32 width = static_cast<u32>(r.read_bits(kWidthBits));
    const u64 zz    = r.read_bits(width);
    const i64 delta = zigzag_decode(zz);
    return static_cast<i32>(static_cast<i64>(prev) + delta);
}

}  // namespace

void encode_bit_delta(std::span<const EntityState> prev,
                      std::span<const EntityState> curr,
                      f32                          pos_resolution_m,
                      std::vector<u8>&             out) {
    out.clear();

    // Quantize + id-sort both sides into local scratch. Sorted order lets us
    // walk prev and curr with a single merge, and makes the bit stream depend
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
            while (j < qc.size() && qc[j].id < qp[i].id) ++j;
            if (j < qc.size() && qc[j].id == qp[i].id) {
                ++i;  // present in both — not removed
            } else {
                removed.push_back(qp[i].id);
                ++i;
            }
        }
    }

    // ── Pass 2: changed records — each curr entity that is NEW (id absent from
    // prev) OR whose quantized record differs from its prev counterpart, in
    // ascending id order (qc is already id-sorted). Gathered first so we can
    // write the count header before the records. ─────────────────────────────
    std::vector<const QuantizedState*> changed;  // points into qc (stable)
    std::vector<const QuantizedState*> base_of;   // matching prev, or nullptr
    changed.reserve(qc.size());
    base_of.reserve(qc.size());
    {
        usize i = 0;  // index into qp
        for (const QuantizedState& c : qc) {
            while (i < qp.size() && qp[i].id < c.id) ++i;
            const bool has_prev = (i < qp.size() && qp[i].id == c.id);
            if (has_prev && quantized_equal(qp[i], c)) {
                continue;  // unchanged — omit from the delta
            }
            changed.push_back(&c);
            base_of.push_back(has_prev ? &qp[i] : nullptr);
        }
    }

    // ── Emit the bit stream. ─────────────────────────────────────────────────
    BitWriter w;
    w.write_bits(static_cast<u64>(removed.size()), kCountBits);
    for (const u32 id : removed) {
        w.write_bits(static_cast<u64>(id), kIdBits);
    }

    w.write_bits(static_cast<u64>(changed.size()), kCountBits);
    for (usize k = 0; k < changed.size(); ++k) {
        const QuantizedState& c = *changed[k];
        const QuantizedState* b = base_of[k];  // nullptr => new entity, prev = 0
        w.write_bits(static_cast<u64>(c.id), kIdBits);
        write_field(w, c.pos[0],    b ? b->pos[0]    : 0);
        write_field(w, c.pos[1],    b ? b->pos[1]    : 0);
        write_field(w, c.pos[2],    b ? b->pos[2]    : 0);
        write_field(w, c.yaw_milli, b ? b->yaw_milli : 0);
    }

    w.flush();
    out = w.bytes();
}

bool decode_bit_delta(std::span<const EntityState> prev,
                      std::span<const u8>          bytes,
                      f32                          pos_resolution_m,
                      std::vector<EntityState>&    out) {
    // Quantize + id-sort the baseline; this is the starting point we mutate. We
    // build the reconstruction in a local vector and only touch `out` once the
    // reader is confirmed to have stayed in range — a malformed buffer leaves
    // `out` untouched.
    std::vector<QuantizedState> recs;
    quantize_sorted(prev, pos_resolution_m, recs);

    BitReader r(bytes);

    // Total bits the buffer can actually supply. We never reserve() against a
    // declared count larger than what these bits could hold — a corrupt/oversized
    // count must not provoke a giant pre-parse allocation; it simply overruns and
    // trips ok() during the read instead.
    const usize avail_bits = bytes.size() * usize{8};

    // ── Removed list. ────────────────────────────────────────────────────────
    const u32 removed_count = static_cast<u32>(r.read_bits(kCountBits));
    if (static_cast<usize>(removed_count) * kIdBits > avail_bits) return false;
    std::vector<u32> removed_ids;
    removed_ids.reserve(removed_count);
    for (u32 i = 0; i < removed_count; ++i) {
        removed_ids.push_back(static_cast<u32>(r.read_bits(kIdBits)));
        if (!r.ok()) return false;  // bail early on an obviously oversized count
    }

    // ── Changed list. ────────────────────────────────────────────────────────
    const u32 changed_count = static_cast<u32>(r.read_bits(kCountBits));
    // Smallest possible record = id + four zero-width fields (each a 6-bit
    // header, no payload). Reject a declared count that cannot fit before we
    // reserve, so a corrupt count overruns harmlessly rather than over-allocating.
    constexpr usize kMinRecordBits = kIdBits + 4 * kWidthBits;
    if (static_cast<usize>(changed_count) * kMinRecordBits > avail_bits) return false;

    // Collect parsed records before mutating `recs`; if the reader overruns
    // mid-parse we abandon everything without disturbing `out`.
    std::vector<QuantizedState> parsed;
    parsed.reserve(changed_count);
    for (u32 c = 0; c < changed_count; ++c) {
        const u32 id = static_cast<u32>(r.read_bits(kIdBits));
        // The baseline for each field is the matching prev record (zero if new).
        const auto it = std::lower_bound(
            recs.begin(), recs.end(), id,
            [](const QuantizedState& s, u32 want) { return s.id < want; });
        const bool has_base = (it != recs.end() && it->id == id);

        QuantizedState q{};
        q.id        = id;
        q.pos[0]    = read_field(r, has_base ? it->pos[0]    : 0);
        q.pos[1]    = read_field(r, has_base ? it->pos[1]    : 0);
        q.pos[2]    = read_field(r, has_base ? it->pos[2]    : 0);
        q.yaw_milli = read_field(r, has_base ? it->yaw_milli : 0);
        parsed.push_back(q);

        if (!r.ok()) return false;  // overran while parsing this record
    }

    if (!r.ok()) return false;  // any latched overrun — buffer was truncated

    // Buffer fully parsed and in range — now it is safe to mutate.
    // Drop the removed ids (linear scan; the scaffold keeps baselines small,
    // matching SnapshotReplication's find_state TODO note).
    for (const u32 id : removed_ids) {
        for (usize k = 0; k < recs.size(); ++k) {
            if (recs[k].id == id) {
                recs.erase(recs.begin() + static_cast<std::ptrdiff_t>(k));
                break;
            }
        }
    }

    // Overlay each parsed record: replace a matching id, else insert in order so
    // `recs` stays id-sorted and the final emit is ascending without a re-sort.
    for (const QuantizedState& q : parsed) {
        const auto it = std::lower_bound(
            recs.begin(), recs.end(), q.id,
            [](const QuantizedState& s, u32 want) { return s.id < want; });
        if (it != recs.end() && it->id == q.id) {
            *it = q;  // replace existing record
        } else {
            recs.insert(it, q);  // new id — insert in order
        }
    }

    out.clear();
    out.reserve(recs.size());
    for (const QuantizedState& q : recs) {
        out.push_back(dequantize_state(q, pos_resolution_m));
    }
    return true;
}

}  // namespace psynder::net
