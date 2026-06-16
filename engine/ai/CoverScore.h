// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/CoverScore.h
//
// Tactical cover-position evaluator — the "where do I take cover?" scorer. Given
// a set of candidate cover positions (e.g. produced by CoverPoints.h and lifted
// to world space), a set of threats (enemy positions), and a desired anchor (an
// objective the agent wants to hold, or the agent's own position), score each
// candidate and pick the best. A good cover spot is FAR from the nearest threat
// (the most dangerous enemy) and CLOSE to the anchor: you want to be shielded
// from fire yet still useful to the objective.
//
// Scoring formula (documented once, applied everywhere). For a candidate c with
// nearest-threat distance d_threat = min over threats of |c - threat| (3D
// Euclidean) and anchor distance d_anchor = |c - anchor|:
//
//     reward = max(d_threat, min_threat_dist_m)
//     score  = threat_weight * reward  -  anchor_weight * d_anchor
//
// The clamp `max(d_threat, min_threat_dist_m)` is a FLOOR on the reward: it does
// NOT cap how safe a far candidate is (bigger d_threat keeps scoring higher) —
// it floors how the reward behaves so that a candidate sitting essentially on top
// of a threat (d_threat ~ 0) is rewarded as if it were `min_threat_dist_m` away
// rather than collapsing the reward to 0. (The name "min_threat_dist_m" is read
// as: the minimum threat distance the reward will credit.) The penalty term pulls
// the agent back toward the anchor; with a sane positive anchor_weight a spot
// far from the objective is dispreferred even if it is very safe.
//
// No-threats case: when `threats` is empty there is no nearest threat, so the
// threat term is a documented large safe constant kNoThreatReward (every position
// is maximally safe); the score then reduces to
// `threat_weight * kNoThreatReward - anchor_weight * d_anchor`, i.e. pure
// pull-to-anchor — the closest candidate to the anchor wins.
//
// Determinism (ai lane, lockstep pillar): pure +,-,*, comparisons and a single
// std::sqrt per distance (via math::length); no RNG, no trig. Ties (equal score)
// always break to the LOWEST candidate index, so the same inputs yield the same
// pick on every run/platform. Built -fno-fast-math.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <span>

namespace psynder::ai {

// Weights for the cover-position score. Reward being far from the nearest
// threat (threat_weight), penalize being far from the anchor (anchor_weight),
// and floor the threat reward at min_threat_dist_m so a threat the agent is
// standing on does not dominate the score to ~0 (see the formula in the header
// comment). All distances are in metres (1 world unit = 1 metre).
struct CoverWeights {
    f32 threat_weight;      // reward per metre of nearest-threat distance
    f32 anchor_weight;      // penalty per metre of distance to the anchor
    f32 min_threat_dist_m;  // floor (metres) on the threat-distance reward term
};

// Sensible default: safety from the nearest threat matters a bit more than
// hugging the anchor, with a 2 m floor so near-threat candidates aren't credited
// below "2 m of clearance".
inline constexpr CoverWeights kDefaultCoverWeights{
    /*threat_weight   =*/ 1.0f,
    /*anchor_weight   =*/ 0.5f,
    /*min_threat_dist_m=*/ 2.0f,
};

// The threat-reward used when there are no threats at all: a large "everywhere is
// safe" constant so the score degenerates to pure pull-to-anchor. Chosen well
// above any realistic per-metre threat distance on a single map.
inline constexpr f32 kNoThreatReward = 1.0e6f;

// Score one candidate cover position against the threats and the anchor; higher
// is better. See the file header for the exact formula and the no-threats rule.
// Pure algebra + sqrt; deterministic.
f32 cover_score(math::Vec3 candidate, std::span<const math::Vec3> threats,
                math::Vec3 anchor, const CoverWeights& w) noexcept;

// Pick the highest-scoring candidate. Writes the chosen index to `out_index` and
// returns true. Equal-score ties break to the LOWEST index. When `candidates` is
// empty there is nothing to pick: returns false and leaves `out_index` untouched.
// Deterministic.
bool best_cover(std::span<const math::Vec3> candidates,
                std::span<const math::Vec3> threats, math::Vec3 anchor,
                const CoverWeights& w, usize& out_index) noexcept;

}  // namespace psynder::ai
