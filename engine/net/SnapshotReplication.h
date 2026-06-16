// SPDX-License-Identifier: MIT
// Psynder-GX — server-authoritative snapshot replication (delta codec).
// Lane 18 (net). Issue #40, ADR-020.
//
// Server-authoritative replication streams entity state to clients each tick.
// To keep bandwidth bounded we only ship what changed since the client's last
// acked baseline: a per-entity delta. This header is a SCAFFOLD — the field
// set is the minimal {id, pos[3]} POD; richer schemas (rotation, anim, game
// bits) land with the gameplay layer in Wave B.
//
// DOTS contract: EntityState is POD / trivially-copyable / SoA-friendly. The
// codec writes into a caller-owned buffer so the steady state performs no
// per-frame heap allocation (callers reserve once and reuse). No exceptions,
// no RTTI in the hot encode/apply path.
//
// Wire encoding (one delta payload):
//   header : u32 count                       — number of changed-entity records
//   record : u32 id, f32 pos[3], f32 yaw_deg — full state of each changed entity
//
// "Changed" = an entity present in `current` whose id is absent from the
// baseline OR whose position/yaw differs bit-exactly from its baseline entry.
// Entities identical to the baseline are omitted. Bit-exact (memcmp) equality
// is intentional: lockstep determinism forbids float epsilon fuzz here.
//
// Determinism: net TUs build with -fno-fast-math / -ffp-contract=off
// (cmake/HotLane.cmake, DESIGN-PSYNDER-GX.md §14). Records are emitted in the
// iteration order of `current`, giving a stable, tick-reproducible byte stream.

#pragma once

#include "core/Types.h"

#include <cstring>
#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// EntityState — minimal replicated entity record. POD, trivially copyable,
// 20 bytes (u32 id + 3 * f32 pos + f32 yaw_deg). SoA-friendly: an array packs
// with no padding. `yaw_deg` is the replicated facing in degrees (1 unit = 1
// metre for pos; angles in degrees, matching the rest of the engine).
// ──────────────────────────────────────────────────────────────────────────
struct EntityState {
    u32 id      = 0;
    f32 pos[3]  = {0.f, 0.f, 0.f};
    f32 yaw_deg = 0.f;
};

static_assert(std::is_trivially_copyable_v<EntityState>,
              "EntityState must be trivially copyable for the DOTS/SoA contract");
static_assert(sizeof(EntityState) == 20, "EntityState wire size drifted");

// Bytes per changed-entity record on the wire: u32 id + 3 * f32 pos + f32 yaw.
inline constexpr usize kDeltaRecordBytes =
    sizeof(u32) + 3 * sizeof(f32) + sizeof(f32);  // 20
inline constexpr usize kDeltaHeaderBytes = sizeof(u32);                    // 4

// Upper bound on the encoded size for `n` changed entities. Callers use this
// to reserve `out` once so the steady-state path never reallocates.
inline constexpr usize max_delta_size(usize changed_count) noexcept {
    return kDeltaHeaderBytes + changed_count * kDeltaRecordBytes;
}

// Locate `id` in `baseline`. Returns nullptr if absent. Linear scan: the
// scaffold keeps baselines small; TODO(ADR-020/#40) index by id when the
// replicated set grows (sorted-by-id baseline + binary search, or a side map).
inline const EntityState* find_state(std::span<const EntityState> baseline,
                                     u32 id) noexcept {
    for (const EntityState& s : baseline) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

inline bool state_equal(const EntityState& a, const EntityState& b) noexcept {
    // Bit-exact compare — lockstep determinism forbids epsilon fuzz. Compare the
    // whole replicated payload (pos + yaw) bit-for-bit.
    return a.id == b.id &&
           std::memcmp(a.pos, b.pos, sizeof(a.pos)) == 0 &&
           std::memcmp(&a.yaw_deg, &b.yaw_deg, sizeof(a.yaw_deg)) == 0;
}

// ──────────────────────────────────────────────────────────────────────────
// encode_delta — append a delta of `current` against `baseline` to `out`.
// Existing bytes in `out` are preserved (the caller may pack a header before
// the delta). Only entities new-or-changed relative to `baseline` are emitted.
// Returns the number of bytes appended.
// ──────────────────────────────────────────────────────────────────────────
inline usize encode_delta(std::span<const EntityState> baseline,
                          std::span<const EntityState> current,
                          std::vector<u8>&             out) noexcept {
    const usize start = out.size();

    // Reserve the count slot; back-patched once the changed count is known.
    const usize count_at = out.size();
    for (usize i = 0; i < kDeltaHeaderBytes; ++i) out.push_back(0);

    u32 changed = 0;
    for (const EntityState& cur : current) {
        const EntityState* base = find_state(baseline, cur.id);
        if (base != nullptr && state_equal(*base, cur)) {
            continue;  // unchanged — omit from the delta
        }
        const u8* bytes = reinterpret_cast<const u8*>(&cur.id);
        out.insert(out.end(), bytes, bytes + sizeof(u32));
        const u8* pos = reinterpret_cast<const u8*>(cur.pos);
        out.insert(out.end(), pos, pos + 3 * sizeof(f32));
        const u8* yaw = reinterpret_cast<const u8*>(&cur.yaw_deg);
        out.insert(out.end(), yaw, yaw + sizeof(f32));
        ++changed;
    }

    std::memcpy(out.data() + count_at, &changed, sizeof(changed));
    return out.size() - start;
}

// ──────────────────────────────────────────────────────────────────────────
// apply_delta — reconstruct the post-delta entity set into `out`. Starts from
// `baseline`, overlays each changed record (replacing matching ids, appending
// new ids). `out` is cleared then filled; the caller may reserve() it ahead of
// time to avoid the alloc. Returns true on success, false if `delta` is
// truncated (header missing or a record runs past the buffer end).
// ──────────────────────────────────────────────────────────────────────────
inline bool apply_delta(std::span<const EntityState> baseline,
                        std::span<const u8>          delta,
                        std::vector<EntityState>&    out) noexcept {
    if (delta.size() < kDeltaHeaderBytes) return false;

    u32 count = 0;
    std::memcpy(&count, delta.data(), sizeof(count));

    const usize need = kDeltaHeaderBytes + usize(count) * kDeltaRecordBytes;
    if (delta.size() < need) return false;  // truncated stream

    out.clear();
    out.insert(out.end(), baseline.begin(), baseline.end());

    usize off = kDeltaHeaderBytes;
    for (u32 i = 0; i < count; ++i) {
        EntityState rec{};
        std::memcpy(&rec.id, delta.data() + off, sizeof(u32));
        off += sizeof(u32);
        std::memcpy(rec.pos, delta.data() + off, 3 * sizeof(f32));
        off += 3 * sizeof(f32);
        std::memcpy(&rec.yaw_deg, delta.data() + off, sizeof(f32));
        off += sizeof(f32);

        bool replaced = false;
        for (EntityState& s : out) {
            if (s.id == rec.id) { s = rec; replaced = true; break; }
        }
        if (!replaced) out.push_back(rec);
    }
    return true;
}

}  // namespace psynder::net
