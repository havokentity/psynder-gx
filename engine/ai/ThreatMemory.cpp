// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/ThreatMemory.cpp — see ThreatMemory.h.

#include "ai/ThreatMemory.h"

namespace psynder::ai {

ThreatMemory::ThreatMemory(u32 capacity)
    : slots_(static_cast<usize>(capacity)) {
    // vector default-constructs ThreatEntry{}; force the invariant explicitly so
    // a default ThreatEntry (active == false here) can never read as live.
    for (ThreatEntry& e : slots_) {
        e.enemy_id = 0u;
        e.last_pos = {0.0f, 0.0f, 0.0f};
        e.age_s    = 0.0f;
        e.active   = false;
    }
}

void ThreatMemory::clear() noexcept {
    for (ThreatEntry& e : slots_) e.active = false;
}

void ThreatMemory::record(u32 enemy_id, math::Vec3 pos) noexcept {
    // 1) Reuse an existing slot for this enemy (refresh in place, no duplicate).
    for (ThreatEntry& e : slots_) {
        if (e.active && e.enemy_id == enemy_id) {
            e.last_pos = pos;
            e.age_s    = 0.0f;
            return;
        }
    }

    // 2) Otherwise take a slot for a NEW enemy. Prefer the lowest-index free
    //    slot; if none is free, evict the OLDEST active entry (largest age_s,
    //    ties -> lowest slot index). A single forward scan finds both: the first
    //    inactive slot wins outright, else we track the max-age active slot.
    const usize n = slots_.size();
    if (n == 0) return;  // capacity 0: nothing to remember in.

    usize target = n;  // sentinel: no free slot found yet
    usize oldest = 0;
    f32   oldest_age = -1.0f;
    for (usize i = 0; i < n; ++i) {
        const ThreatEntry& e = slots_[i];
        if (!e.active) {
            target = i;  // lowest-index free slot
            break;
        }
        // Strictly-greater keeps the FIRST (lowest index) slot on an age tie.
        if (e.age_s > oldest_age) {
            oldest_age = e.age_s;
            oldest     = i;
        }
    }
    if (target == n) target = oldest;  // full: evict the oldest

    ThreatEntry& slot = slots_[target];
    slot.enemy_id = enemy_id;
    slot.last_pos = pos;
    slot.age_s    = 0.0f;
    slot.active   = true;
}

void ThreatMemory::tick(f32 dt_s, f32 forget_after_s) noexcept {
    for (ThreatEntry& e : slots_) {
        if (!e.active) continue;
        e.age_s += dt_s;
        // Strictly-greater: an entry exactly at the timeout still counts as
        // remembered this tick and is forgotten only once it ages past it.
        if (e.age_s > forget_after_s) e.active = false;
    }
}

const ThreatEntry* ThreatMemory::find(u32 enemy_id) const noexcept {
    for (const ThreatEntry& e : slots_) {
        if (e.active && e.enemy_id == enemy_id) return &e;
    }
    return nullptr;
}

bool ThreatMemory::freshest(ThreatEntry& out) const noexcept {
    const ThreatEntry* best = nullptr;
    for (const ThreatEntry& e : slots_) {
        if (!e.active) continue;
        if (best == nullptr || e.age_s < best->age_s ||
            (e.age_s == best->age_s && e.enemy_id < best->enemy_id)) {
            // Smallest age wins; on an exact age tie the lower enemy_id wins.
            best = &e;
        }
    }
    if (best == nullptr) return false;
    out = *best;
    return true;
}

u32 ThreatMemory::active_count() const noexcept {
    u32 count = 0;
    for (const ThreatEntry& e : slots_) {
        if (e.active) ++count;
    }
    return count;
}

}  // namespace psynder::ai
