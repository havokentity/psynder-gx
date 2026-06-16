// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Reload.cpp — see Reload.h.

#include "gameplay/Reload.h"

#include "gameplay/GameplayComponents.h"  // (sibling component header)

namespace psynder::gameplay {

bool can_fire(const Magazine& m) noexcept {
    return m.in_mag > 0 && m.reload_left_s <= 0.0f;
}

bool consume_round(Magazine& m) noexcept {
    if (!can_fire(m)) return false;
    m.in_mag -= 1;
    return true;
}

bool start_reload(Magazine& m) noexcept {
    if (m.reload_left_s > 0.0f) return false;     // already reloading
    if (m.in_mag >= m.mag_size) return false;     // mag already full
    if (m.reserve <= 0) return false;             // nothing to reload from
    m.reload_left_s = m.reload_time_s;
    return true;
}

bool reloading(const Magazine& m) noexcept {
    return m.reload_left_s > 0.0f;
}

void tick_reload(Magazine& m, f32 dt_s) noexcept {
    if (m.reload_left_s <= 0.0f) return;  // not reloading
    m.reload_left_s -= dt_s;
    if (m.reload_left_s <= 0.0f) {
        m.reload_left_s = 0.0f;
        const i32 missing = m.mag_size - m.in_mag;
        const i32 moved = (missing < m.reserve) ? missing : m.reserve;
        if (moved > 0) {
            m.in_mag += moved;
            m.reserve -= moved;
        }
    }
}

void tick_reloads(scene::World& w, f32 dt_s) noexcept {
    // Pure per-entity update (no cross-entity state, no structural changes), so
    // the chunk walk is order-independent and deterministic.
    w.for_each_chunk<Magazine>([&](usize n, Magazine* mags) {
        for (usize i = 0; i < n; ++i) tick_reload(mags[i], dt_s);
    });
}

}  // namespace psynder::gameplay
