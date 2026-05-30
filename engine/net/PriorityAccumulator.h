// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — per-entity send-priority accumulator. Lane 18 (net).
//
// Area-of-interest (Aoi.h / InterestManagement.h) answers WHICH entities are
// *relevant* to a peer this tick. But on a busy frame the relevant set can still
// blow the packet budget — a 64-byte MTU snapshot can carry only so many entity
// records. This accumulator answers the next question: of the relevant entities,
// WHICH ones do we actually ship THIS tick, and which wait their turn?
//
// It is the classic Glenn-Fiedler priority-accumulator scheme. Each entity has a
// base priority (how badly it wants to be sent — e.g. scaled by how close it is,
// how fast it is moving, whether it just fired). Every tick its *accumulated*
// priority grows by its base. To send a snapshot we pick the top-K (the packet
// budget) by accumulated priority and RESET those to 0. An entity that loses the
// race this tick keeps its accumulated priority and so climbs the queue every
// tick it misses, until it eventually wins — no entity is ever starved. A higher
// base climbs faster (sent more often); a base of 0 never rises on its own and is
// never sent unprompted.
//
// SCOPE: this is a *scheduler*, not authoritative sim state. The selection it
// makes drives which records the replication layer packs, but it never feeds back
// into the lockstep simulation, so it cannot change the deterministic game
// outcome. It still lives in the strict-FP net lane (cmake/HotLane.cmake builds
// it -fno-fast-math / -ffp-contract=off, DESIGN-PSYNDER-GX.md §14), so for a
// given op sequence the selection and the accumulator state are bit-reproducible
// across runs and platforms.
//
// Determinism: pure f32 algebra, no transcendentals, no RNG. Ties in accumulated
// priority break to the LOWEST entity index, and the selected output is ordered
// by descending priority (ties ascending index), so the same op sequence always
// yields the same selection and the same residual state. No per-call heap growth
// beyond the caller's out vector — the selection scratch is a reused member.

#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// PriorityAccumulator — a flat per-entity accumulator indexed [0, size()). The
// entity-index space is dense and caller-owned (the AoI / replication layer maps
// its entity ids onto these slots); this class only knows the accumulators.
// NOT thread-safe: select() mutates the shared scratch buffer, so a single
// accumulator must be driven from one thread at a time.
// ──────────────────────────────────────────────────────────────────────────
class PriorityAccumulator {
public:
    PriorityAccumulator() noexcept = default;

    // Set the per-entity accumulator count, zeroing every accumulator. Sizing the
    // scratch up-front here keeps select() allocation-free in the hot path.
    void resize(usize entity_count);

    // Zero every accumulator without changing size().
    void reset() noexcept;

    // Advance one tick: for each entity i (over min(span, size()) entities),
    // acc[i] += max(0, base_priorities[i]). A higher base climbs faster; a base
    // of 0 (or a clamped-away negative) leaves the accumulator unchanged so a
    // zero-priority entity never rises on its own. Pure algebra, no allocation.
    void accumulate(std::span<const f32> base_priorities) noexcept;

    // Pick the up-to-max_count entities with the HIGHEST accumulated priority and
    // write their indices to out_indices, ordered by DESCENDING priority (ties
    // broken ASCENDING index). Entities with acc <= 0 are never selected — there
    // is nothing to send for them — so fewer than max_count may be returned (or
    // zero). The selected entities' accumulators are RESET to 0; every other
    // accumulator keeps its value and so climbs further next tick (anti-
    // starvation). out_indices is cleared first; the internal ranking scratch is
    // reused (no per-call heap growth beyond out_indices).
    void select(usize max_count, std::vector<u32>& out_indices) noexcept;

    // Current accumulated priority of entity i, or 0 if i is out of range.
    f32 priority(usize i) const noexcept;

    // Number of accumulators (the dense entity-index space).
    usize size() const noexcept { return acc_.size(); }

private:
    std::vector<f32> acc_;      // per-entity accumulated priority, [0, size()).
    std::vector<u32> scratch_;  // reused candidate-index buffer for select().
};

}  // namespace psynder::net
