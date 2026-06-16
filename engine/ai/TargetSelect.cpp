// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/TargetSelect.cpp — see TargetSelect.h.

#include "ai/TargetSelect.h"

namespace psynder::ai {

namespace {
// Branchless clamp to [0, 1] (no <algorithm> needed; keeps the math obvious).
constexpr f32 clamp01(f32 v) noexcept {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
}  // namespace

f32 target_score(math::Vec3 chooser_pos, math::Vec3 cand_pos, f32 cand_health,
                 bool visible, const TargetWeights& w) noexcept {
    const f32 d = math::length(math::sub(cand_pos, chooser_pos));  // one sqrt
    const f32 hfrac = clamp01(cand_health / 100.0f);               // 1 full, 0 dead
    return w.visible_bonus * (visible ? 1.0f : 0.0f)
         - w.distance_weight * d
         + w.low_health_bonus * (1.0f - hfrac);
}

bool select_target(math::Vec3 chooser_pos, u32 chooser_team,
                   std::span<const TargetCandidate> candidates,
                   const TargetWeights& w, u32& out_id) noexcept {
    bool found = false;
    f32  best_score = 0.0f;
    u32  best_id = 0u;

    for (const TargetCandidate& c : candidates) {
        if (c.team == chooser_team) continue;  // never target a teammate

        // Hard range gate: square the comparison so out-of-range candidates cost
        // no sqrt, and ones in range reuse the same distance the score needs.
        const math::Vec3 delta = math::sub(c.pos, chooser_pos);
        const f32 dist2 = math::dot(delta, delta);
        if (dist2 > w.max_range_m * w.max_range_m) continue;

        const f32 s = target_score(chooser_pos, c.pos, c.health, c.visible, w);

        // Strictly-greater claims the lead; an exact tie keeps the LOWER id (we
        // only overwrite a tie when the new id is smaller) -> deterministic.
        if (!found || s > best_score || (s == best_score && c.id < best_id)) {
            found = true;
            best_score = s;
            best_id = c.id;
        }
    }

    if (found) out_id = best_id;
    return found;
}

}  // namespace psynder::ai
