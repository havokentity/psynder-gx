// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Killfeed.cpp — see Killfeed.h.
//
// Deterministic: events live newest-first in one bounded vector. push() inserts
// at the front; when full it drops the back first (the oldest). tick() ages then
// compacts. No RNG, no per-call heap alloc — capacity is reserved in the ctor and
// the vector never exceeds it.

#include "gameplay/Killfeed.h"

namespace psynder::gameplay {

Killfeed::Killfeed(u32 capacity) : capacity_(capacity < 1u ? usize{1} : usize{capacity}) {
    // Reserve once so no push/tick ever reallocates the storage.
    events_.reserve(capacity_);
}

void Killfeed::clear() noexcept {
    events_.clear();  // keeps the reserved capacity
}

void Killfeed::push(Entity killer, Entity victim, u32 weapon_class) noexcept {
    // Make room by evicting the oldest (the back) when at capacity.
    if (events_.size() >= capacity_) {
        events_.pop_back();
    }
    // Newest-first: the fresh event goes to the front at age 0.
    events_.insert(events_.begin(), KillEvent{killer, victim, weapon_class, 0.0f});
}

void Killfeed::tick(f32 dt_s, f32 max_age_s) noexcept {
    // Age every event, then drop those strictly past max_age. Because the feed
    // is newest-first, all survivors form a contiguous prefix once aged (older
    // events are at the back), so a single truncation suffices and the result is
    // independent of iteration order.
    usize live = 0;
    for (usize i = 0; i < events_.size(); ++i) {
        events_[i].age_s += dt_s;
        if (events_[i].age_s <= max_age_s) {
            ++live;
        }
    }
    // Survivors (age <= max_age) are exactly the `live` newest entries: a younger
    // event can never age past max_age while an older (larger-age) one survives,
    // so the kept set is the front prefix. Truncate the aged-out tail.
    if (live < events_.size()) {
        events_.resize(live);
    }
}

bool Killfeed::newest(KillEvent& out) const noexcept {
    if (events_.empty()) {
        return false;
    }
    out = events_.front();
    return true;
}

}  // namespace psynder::gameplay
