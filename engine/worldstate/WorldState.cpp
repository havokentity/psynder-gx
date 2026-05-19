// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — OpaqueWorldState producer implementation.
//
// Compiled with `-fno-fast-math -ffp-contract=off` per DESIGN §14 (and the
// project-memory "real-physics" rule); see the CMakeLists.txt for this lane.
// That flag set is load-bearing: lockstep / demo replay requires bit-identical
// lerp output across platforms and compiler versions, which fast-math /
// fused-multiply-add break.

#include "WorldState.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace psynder::worldstate {

namespace {

// Memcpy-based unaligned reader. The wire format is dense-packed and not
// guaranteed to be naturally aligned to the start of `bytes` if the caller
// did anything unusual. memcpy compiles to a single mov on x86/ARM for these
// POD sizes, so this costs nothing and avoids UB.
template <class T>
PSY_FORCEINLINE T read_pod(const u8* p) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <class T>
PSY_FORCEINLINE void write_pod(u8* p, const T& v) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(p, &v, sizeof(T));
}

// Locate the record in `records` whose entity_id + archetype_hash matches
// `key`. Returns nullptr on miss or on archetype mismatch (which is a hard
// error per the interpolate_world_state contract).
const EntityRecord* find_match(const EntityRecord*  records,
                               usize                count,
                               u32                  id,
                               u32                  archetype) noexcept {
    for (usize i = 0; i < count; ++i) {
        if (records[i].entity_id == id) {
            if (records[i].archetype_hash != archetype) {
                return nullptr;  // archetype changed mid-stream — reject.
            }
            return &records[i];
        }
    }
    return nullptr;
}

// Single-entity blend used by both the serial and parallel paths.
PSY_FORCEINLINE void lerp_one(const EntityRecord& a,
                              const EntityRecord& b,
                              f32                 t,
                              EntityRecord*       out) noexcept {
    out->entity_id      = a.entity_id;
    out->archetype_hash = a.archetype_hash;
    // Standard form `a + (b - a) * t` is preferred over `a*(1-t) + b*t` for
    // determinism: it avoids the `1.0f - t` cancellation when t is near 1
    // and keeps the per-platform rounding identical with -ffp-contract=off.
    out->pos_x = a.pos_x + (b.pos_x - a.pos_x) * t;
    out->pos_y = a.pos_y + (b.pos_y - a.pos_y) * t;
    out->pos_z = a.pos_z + (b.pos_z - a.pos_z) * t;
    out->yaw_y = a.yaw_y + (b.yaw_y - a.yaw_y) * t;
    out->vel_x = a.vel_x + (b.vel_x - a.vel_x) * t;
    out->vel_y = a.vel_y + (b.vel_y - a.vel_y) * t;
    out->vel_z = a.vel_z + (b.vel_z - a.vel_z) * t;
    // Bitfield flags don't lerp; snap to the nearer endpoint, ties → `a` so
    // the result stays deterministic across re-runs.
    out->flags = (t < 0.5f) ? a.flags : b.flags;
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────────
void write_world_state(std::span<const EntityRecord> in,
                       std::span<u8>                 out) noexcept {
    assert(in.size() <= kMaxEntities &&
           "world-state entity_count exceeds kMaxEntities");
    assert(out.size() == compute_world_state_bytes(in.size()) &&
           "write_world_state out buffer wrong size");

    WorldStateHeader hdr{};
    hdr.magic        = kWorldStateMagic;
    hdr.version      = kWorldStateVersion;
    hdr.entity_count = static_cast<u16>(in.size());
    hdr.reserved0    = 0;
    hdr.reserved1    = 0;
    write_pod(out.data(), hdr);

    u8* records_out = out.data() + sizeof(WorldStateHeader);
    if (!in.empty()) {
        std::memcpy(records_out,
                    in.data(),
                    in.size() * sizeof(EntityRecord));
    }
}

// ──────────────────────────────────────────────────────────────────────────
bool read_world_state_header(std::span<const u8> bytes,
                             u16*                out_version,
                             u16*                out_entity_count) noexcept {
    if (bytes.size() < sizeof(WorldStateHeader)) return false;

    const auto hdr = read_pod<WorldStateHeader>(bytes.data());
    if (hdr.magic   != kWorldStateMagic)   return false;
    if (hdr.version != kWorldStateVersion) return false;
    if (hdr.entity_count > kMaxEntities)   return false;

    const usize expected = compute_world_state_bytes(hdr.entity_count);
    if (bytes.size() < expected) return false;

    if (out_version)      *out_version      = hdr.version;
    if (out_entity_count) *out_entity_count = hdr.entity_count;
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
bool interpolate_world_state(std::span<const u8> a,
                             std::span<const u8> b,
                             f32                 t,
                             std::span<u8>       out) noexcept {
    u16 va = 0, na = 0, vb = 0, nb = 0;
    if (!read_world_state_header(a, &va, &na)) return false;
    if (!read_world_state_header(b, &vb, &nb)) return false;
    if (va != vb)                              return false;
    if (na != nb)                              return false;
    if (out.size() != a.size())                return false;
    if (out.size() != b.size())                return false;

    // Clamp t into [0,1] up front so the lerp result is well-defined even if
    // the caller's tick-fraction arithmetic underflows or overflows. This is
    // deliberate per the public-header contract.
    const f32 ct = std::clamp(t, 0.0f, 1.0f);

    // Always copy the header from `a` first; lerp only touches the records.
    // Doing this before the loop means an early exit (n == 0) still produces
    // a valid output buffer.
    std::memcpy(out.data(), a.data(), sizeof(WorldStateHeader));

    const usize n = na;
    if (n == 0) return true;

    const auto* a_recs = reinterpret_cast<const EntityRecord*>(
        a.data() + sizeof(WorldStateHeader));
    const auto* b_recs = reinterpret_cast<const EntityRecord*>(
        b.data() + sizeof(WorldStateHeader));
    auto* out_recs = reinterpret_cast<EntityRecord*>(
        out.data() + sizeof(WorldStateHeader));

    // First pass — validate that every entity in `a` has a partner in `b`
    // with matching archetype. We do this up front (not inline with the
    // lerp) so a mismatch produces `out` undisturbed past the header rather
    // than a partially-lerped buffer that the caller might mistake for
    // success.
    for (usize i = 0; i < n; ++i) {
        if (!find_match(b_recs, n, a_recs[i].entity_id, a_recs[i].archetype_hash)) {
            return false;
        }
    }

    // Per-entity lerp. The body has no shared state, so the parallel path
    // is a straight parallel_for; no atomics, no barriers, no false sharing
    // because each chunk writes contiguous EntityRecord slots.
    auto body = [&](usize lo, usize hi) noexcept {
        for (usize i = lo; i < hi; ++i) {
            const EntityRecord& ai = a_recs[i];
            // Re-find in `b` — records-out-of-order is allowed per contract.
            const EntityRecord* bi = find_match(
                b_recs, n, ai.entity_id, ai.archetype_hash);
            // find_match cannot fail here: the validation loop above proved
            // every entity has a partner. Defensive null-check anyway —
            // costs one predictable branch per entity and prevents a UB
            // dereference if a future refactor reorders the validation.
            if (!bi) continue;
            lerp_one(ai, *bi, ct, &out_recs[i]);
        }
    };

    if (n <= kParallelEntityThreshold) {
        body(0, n);
    } else {
        // Grain = threshold/2 so we land roughly 2× over-decomposition,
        // which is the JobSystem's standard parallel_for sweet spot.
        const usize grain = std::max<usize>(1, kParallelEntityThreshold / 2);
        psynder::jobs::JobSystem::Get().parallel_for(0, n, grain, body);
    }
    return true;
}

}  // namespace psynder::worldstate
