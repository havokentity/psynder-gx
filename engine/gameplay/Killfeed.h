// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Killfeed.h — a fixed-capacity ring of recent kill events.
//
// The HUD reads this each frame to draw the "X killed Y" stack that fades out.
// Pure and deterministic: events are ordered strictly by recency (newest first),
// same-tick pushes break ties by insertion order, no RNG, and no per-call heap
// allocation beyond the one bounded vector reserved at construction. On the
// authoritative lockstep tick the same sequence of push/tick ops yields the same
// feed bit-for-bit. The weapon kill path (see Damage.h `damage_credited`) calls
// push; the sim calls tick once per frame to age events out.

#pragma once

#include "scene/World.h"  // Entity (via core/Types.h)

#include "core/Types.h"

#include <vector>

namespace psynder::gameplay {

// One entry in the kill feed. POD. `age_s` is the seconds since the kill landed
// (0 when freshly pushed), advanced by Killfeed::tick. `weapon_class` is an
// opaque u32 the HUD maps to an icon (the weapon/category id that scored it).
struct KillEvent {
    Entity killer;        // who scored the kill
    Entity victim;        // who died
    u32    weapon_class;  // opaque weapon/category id (HUD icon key)
    f32    age_s;         // seconds since the kill (0 = just happened)
};

// A bounded, newest-first log of recent kills.
//
// Storage convention: a single std::vector<KillEvent> kept sorted newest-first
// (index 0 is always the most recent event). push() inserts at the front and,
// when full, drops the back (the oldest); tick() ages every entry then erases
// the aged-out ones. The vector's capacity is reserved once at construction and
// never grows past `capacity`, so no op allocates after the ctor.
class Killfeed {
public:
    // Fixed ring of `capacity` events. capacity must be >= 1; a passed-in 0 is
    // clamped up to 1 (a feed must hold at least one event).
    explicit Killfeed(u32 capacity);

    // Drop every event; capacity is unchanged.
    void clear() noexcept;

    // Record a fresh kill (age 0) as the newest event. When the feed is already
    // full this first evicts the OLDEST event to make room. Newest-first is the
    // read convention: the just-pushed event becomes at(0) / newest().
    void push(Entity killer, Entity victim, u32 weapon_class) noexcept;

    // Age every event by `dt_s` seconds, then drop events whose age now exceeds
    // `max_age_s` (strictly older than max_age survive being == max_age). The
    // surviving events keep their newest-first order; the effect is independent
    // of iteration order. No allocation.
    void tick(f32 dt_s, f32 max_age_s) noexcept;

    // Number of live events currently in the feed (0..capacity).
    usize count() const noexcept { return events_.size(); }

    // The i-th most-recent event (i == 0 is the newest). The caller MUST ensure
    // i < count(); out-of-range access is undefined (no bounds check).
    const KillEvent& at(usize i) const noexcept { return events_[i]; }

    // Copy the most recent event into `out` and return true; return false (and
    // leave `out` untouched) when the feed is empty.
    bool newest(KillEvent& out) const noexcept;

private:
    std::vector<KillEvent> events_;  // sorted newest-first; size() == count()
    usize                  capacity_ = 1;
};

}  // namespace psynder::gameplay
