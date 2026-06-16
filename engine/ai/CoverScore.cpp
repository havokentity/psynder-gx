// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/CoverScore.cpp — see CoverScore.h.

#include "ai/CoverScore.h"

namespace psynder::ai {

namespace {
// Distance (metres) from `candidate` to the NEAREST of the threats — the most
// dangerous enemy. Returns kNoThreatReward when there are no threats so callers
// get the documented "everywhere safe" constant. Scans in fixed index order; the
// running minimum is order-independent, so no tie-break is needed here.
f32 nearest_threat_distance(math::Vec3 candidate,
                            std::span<const math::Vec3> threats) noexcept {
    if (threats.empty()) return kNoThreatReward;
    f32 nearest = math::length(math::sub(candidate, threats[0]));
    for (usize i = 1; i < threats.size(); ++i) {
        const f32 d = math::length(math::sub(candidate, threats[i]));
        if (d < nearest) nearest = d;
    }
    return nearest;
}
}  // namespace

f32 cover_score(math::Vec3 candidate, std::span<const math::Vec3> threats,
                math::Vec3 anchor, const CoverWeights& w) noexcept {
    const f32 d_threat = nearest_threat_distance(candidate, threats);

    // Floor the threat-distance reward: a candidate sitting on top of a threat
    // (d_threat ~ 0) is credited as if it had `min_threat_dist_m` of clearance,
    // so it cannot collapse the reward to 0 and dominate the comparison. A larger
    // d_threat still scores higher — the clamp is a floor, not a cap. (In the
    // no-threats case d_threat is already kNoThreatReward, well above the floor,
    // so this leaves it untouched.)
    const f32 reward = (d_threat > w.min_threat_dist_m) ? d_threat : w.min_threat_dist_m;

    const f32 d_anchor = math::length(math::sub(candidate, anchor));

    return w.threat_weight * reward - w.anchor_weight * d_anchor;
}

bool best_cover(std::span<const math::Vec3> candidates,
                std::span<const math::Vec3> threats, math::Vec3 anchor,
                const CoverWeights& w, usize& out_index) noexcept {
    if (candidates.empty()) return false;  // nothing to pick; leave out_index alone

    usize best_i = 0;
    f32 best_score = cover_score(candidates[0], threats, anchor, w);
    for (usize i = 1; i < candidates.size(); ++i) {
        const f32 s = cover_score(candidates[i], threats, anchor, w);
        if (s > best_score) {  // strictly-greater => first (lowest index) wins ties
            best_score = s;
            best_i = i;
        }
    }
    out_index = best_i;
    return true;
}

}  // namespace psynder::ai
