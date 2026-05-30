// SPDX-License-Identifier: MIT
// Psynder — wraparound-safe 16-bit sequence utilities + a sequence-indexed
// ring (the Gaffer-on-Games "sequence buffer").
//
// This is a reusable, generic networking primitive: a fixed-capacity ring
// whose slots are indexed by `sequence % capacity`, each slot remembering the
// sequence number stored in it so a stale slot (the same index, an earlier
// generation) is never mistaken for a live entry. Reliability/jitter/snapshot
// layers build on top of it (`Reliability` tracks acks the same way but with
// its own 32-bit ring; this header is the generic, header-only version a
// future ack-buffer or fragment-reassembler can share).
//
// 16-bit sequence comparison is modular: `seq_greater(a, b)` treats the two
// values as points on a 65536-wide circle and asks which is "more recent",
// assuming the in-flight window never exceeds 2^15 sequences.
//
// Pure integer arithmetic — no floats, no RNG; deterministic and safe for the
// strict-FP net lane.

#pragma once

#include "core/Types.h"

#include <vector>

namespace psynder::net {

// ─── 16-bit modular sequence comparison ──────────────────────────────────

// True iff `a` is strictly "more recent" than `b` under 16-bit wraparound.
// The classic Gaffer formulation: a is ahead of b if it is the larger value
// and the gap is within half the space, OR it has wrapped past b.
PSY_FORCEINLINE bool seq_greater(u16 a, u16 b) noexcept {
    constexpr u16 kHalf = 0x8000u;  // 32768
    return ((a > b) && (u16(a - b) <= kHalf)) ||
           ((a < b) && (u16(b - a) > kHalf));
}

// Signed shortest distance `a - b` accounting for the wrap, in
// [-32768, 32767]. Positive => a is ahead of b. Because unsigned u16
// subtraction is modular, the bit pattern of `u16(a - b)` reinterpreted as a
// signed i16 already is the shortest-path difference; widen to i32 to return.
PSY_FORCEINLINE i32 seq_diff(u16 a, u16 b) noexcept {
    return static_cast<i32>(static_cast<i16>(static_cast<u16>(a - b)));
}

// ─── SequenceBuffer<T> ────────────────────────────────────────────────────
//
// A ring of `capacity` slots indexed by `seq % capacity`. Each slot stores the
// sequence number that owns it (`entry_seq_`), with a sentinel meaning "empty".
// `insert` advances `latest_` and clears any slots skipped over so a gap can
// never surface stale data left by an earlier generation of the ring.
template <typename T>
class SequenceBuffer {
public:
    // Sentinel stored in `entry_seq_` for an unused slot. 0xFFFF is a valid
    // sequence number, but a slot only validates when `entry_seq_[slot] == seq`
    // AND the slot index matches `seq % capacity`; an empty slot holding the
    // sentinel for an index whose owning sequences are not 0xFFFF can never
    // false-positive. To be fully safe across the whole 16-bit space we also
    // track `has_latest_` so an empty buffer reports nothing as present.
    static constexpr u16 kEmpty = 0xFFFFu;

    explicit SequenceBuffer(u32 capacity)
        : entry_seq_(capacity, kEmpty), entries_(capacity) {
        // capacity must be > 0; an empty ring has no valid slot. We avoid an
        // exception in this hot, header-only primitive and instead clamp to 1.
        if (capacity == 0) {
            entry_seq_.assign(1, kEmpty);
            entries_.assign(1, T{});
        }
    }

    // Clear every slot and forget the latest sequence.
    void reset() noexcept {
        for (auto& s : entry_seq_) s = kEmpty;
        for (auto& e : entries_) e = T{};
        latest_ = 0;
        has_latest_ = false;
    }

    // Claim the slot for `seq`. Returns a pointer to a freshly default-
    // constructed entry to fill, or nullptr if `seq` is too old (more than
    // `capacity` behind the newest sequence seen). When `seq` is newer than the
    // current latest, every slot strictly between the old latest and `seq` is
    // cleared so a later `find` over that gap returns nullptr instead of stale
    // data from a previous wrap.
    T* insert(u16 seq) noexcept {
        const u32 cap = capacity();

        if (!has_latest_) {
            // First insert: seed the latest and claim the slot.
            has_latest_ = true;
            latest_ = seq;
            return claim(seq);
        }

        if (seq_greater(seq, latest_)) {
            // Newer: clear the skipped-over slots (exclusive of `seq`, which we
            // are about to overwrite anyway), then advance latest.
            // Cap the clear span at `cap` — clearing more than the ring holds
            // is wasted work; every slot would be revisited.
            const i32 gap = seq_diff(seq, latest_);  // >= 1
            const u32 span = (static_cast<u32>(gap) > cap)
                                 ? cap
                                 : static_cast<u32>(gap);
            for (u32 i = 1; i <= span; ++i) {
                const u16 s = static_cast<u16>(latest_ + i);
                const u32 slot = s % cap;
                entry_seq_[slot] = kEmpty;
                entries_[slot] = T{};
            }
            latest_ = seq;
            return claim(seq);
        }

        // `seq` is older-or-equal to latest. Reject if it falls outside the
        // ring's window (more than `capacity` behind the newest sequence).
        const i32 behind = seq_diff(latest_, seq);  // >= 0
        if (static_cast<u32>(behind) >= cap) {
            return nullptr;  // too old to store
        }
        return claim(seq);
    }

    // The entry for `seq` if a live slot holds it, else nullptr.
    T* find(u16 seq) noexcept {
        const u32 slot = slot_of(seq);
        if (entry_seq_[slot] == seq && is_in_window(seq)) {
            return &entries_[slot];
        }
        return nullptr;
    }

    const T* find(u16 seq) const noexcept {
        const u32 slot = slot_of(seq);
        if (entry_seq_[slot] == seq && is_in_window(seq)) {
            return &entries_[slot];
        }
        return nullptr;
    }

    bool exists(u16 seq) const noexcept { return find(seq) != nullptr; }

    // Newest sequence inserted. 0 before the first insert (callers should gate
    // on a prior insert; `has_latest()` disambiguates the seq==0 case).
    u16 latest() const noexcept { return latest_; }

    bool has_latest() const noexcept { return has_latest_; }

    u32 capacity() const noexcept { return static_cast<u32>(entry_seq_.size()); }

private:
    u32 slot_of(u16 seq) const noexcept {
        return static_cast<u32>(seq) % capacity();
    }

    // Within the addressable window: not in the future, and not more than
    // `capacity` behind the latest. Guards against a sentinel-cleared slot
    // whose stored seq happens to equal a queried-but-out-of-window seq.
    bool is_in_window(u16 seq) const noexcept {
        if (!has_latest_) return false;
        if (seq_greater(seq, latest_)) return false;  // future
        const i32 behind = seq_diff(latest_, seq);     // >= 0
        return static_cast<u32>(behind) < capacity();
    }

    T* claim(u16 seq) noexcept {
        const u32 slot = slot_of(seq);
        entry_seq_[slot] = seq;
        entries_[slot] = T{};  // default-construct fresh storage to fill
        return &entries_[slot];
    }

    std::vector<u16> entry_seq_;  // per-slot owning sequence (kEmpty == free)
    std::vector<T>   entries_;    // per-slot payload
    u16  latest_ = 0;             // newest sequence inserted
    bool has_latest_ = false;     // false until the first insert
};

}  // namespace psynder::net
