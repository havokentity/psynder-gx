// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Stamina.cpp — see Stamina.h.

#include "gameplay/Stamina.h"

namespace psynder::gameplay {

namespace {
// Clamp x into [lo, hi]. Branchy (not std::clamp) to keep it constexpr-simple
// and identical across compilers under strict-FP.
inline f32 clampf(f32 x, f32 lo, f32 hi) noexcept {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
}  // namespace

bool can_sprint(const Stamina& s) noexcept {
    return s.current > 0.0f;
}

void tick_stamina(Stamina& s, bool sprinting, f32 dt_s) noexcept {
    if (sprinting && s.current > 0.0f) {
        // Drain while sprinting, and hold the regen wait at full so regen does
        // not resume the instant the player stops.
        s.current -= s.drain_per_s * dt_s;
        s.regen_cooldown_s = s.regen_delay_s;
    } else {
        // Not sprinting (or out of stamina): count down the regen delay; once it
        // hits 0, refill toward max.
        if (s.regen_cooldown_s > 0.0f) {
            s.regen_cooldown_s -= dt_s;
            if (s.regen_cooldown_s < 0.0f) s.regen_cooldown_s = 0.0f;
        }
        if (s.regen_cooldown_s <= 0.0f) {
            s.current += s.regen_per_s * dt_s;
        }
    }
    s.current = clampf(s.current, 0.0f, s.max_stamina);
}

void tick_staminas(scene::World& w, f32 dt_s) noexcept {
    // Every Stamina regenerates (treated as not-sprinting). Pure per-entity
    // update over the chunk columns: no entity ids, no cross-entity state and no
    // structural changes, so the result is order-independent / deterministic.
    w.for_each_chunk<Stamina>([dt_s](usize n, Stamina* st) {
        for (usize i = 0; i < n; ++i)
            tick_stamina(st[i], /*sprinting=*/false, dt_s);
    });
}

f32 stamina_fraction(const Stamina& s) noexcept {
    if (s.max_stamina <= 0.0f) return 0.0f;
    return s.current / s.max_stamina;
}

}  // namespace psynder::gameplay
