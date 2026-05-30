// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/MatchRules.cpp — see MatchRules.h. Deterministic match
// orchestration: phase machine, win conditions, spawn selection. Strict FP,
// allocation-free scans, id-ordered tie breaks.

#include "gameplay/MatchRules.h"

#include "gameplay/GameplayComponents.h"

#include "scene/GxComponents.h"  // TransformWS

#include <limits>

namespace psynder::gameplay {

Entity frag_leader(scene::World& w, u32* out_frags) noexcept {
    Entity best{};
    u32 best_frags = 0;
    bool found = false;
    w.for_each_chunk_with_entities<Score>(
        [&](usize n, const Entity* ents, Score* sc) {
            for (usize i = 0; i < n; ++i) {
                const u32 f = sc[i].frags;
                const Entity e = ents[i];
                if (!found || f > best_frags ||
                    (f == best_frags && e.raw < best.raw)) {
                    best = e;
                    best_frags = f;
                    found = true;
                }
            }
        });
    if (out_frags != nullptr) *out_frags = found ? best_frags : 0u;
    return best;
}

void tick_match(MatchState& m, scene::World& w, const MatchConfig& cfg,
                f32 dt) noexcept {
    m.just_ended = false;
    m.time_in_phase_s += dt;

    switch (m.phase) {
        case MatchPhase::Warmup: {
            if (m.time_in_phase_s >= cfg.warmup_s) {
                m.phase = MatchPhase::Active;
                m.time_in_phase_s = 0.0f;
                m.match_time_s = 0.0f;
                m.winner = Entity{};
                ++m.round;
            }
            break;
        }
        case MatchPhase::Active: {
            m.match_time_s += dt;
            u32 lead_frags = 0;
            const Entity leader = frag_leader(w, &lead_frags);
            bool ended = false;
            if (cfg.frag_limit > 0u && leader.raw != 0u &&
                lead_frags >= cfg.frag_limit) {
                ended = true;
            }
            if (cfg.time_limit_s > 0.0f && m.match_time_s >= cfg.time_limit_s) {
                ended = true;
            }
            if (ended) {
                m.winner = leader;  // highest frags, ties -> lowest id
                m.phase = MatchPhase::Intermission;
                m.time_in_phase_s = 0.0f;
                m.just_ended = true;
            }
            break;
        }
        case MatchPhase::Intermission: {
            if (m.time_in_phase_s >= cfg.intermission_s) {
                m.phase = MatchPhase::Warmup;
                m.time_in_phase_s = 0.0f;
                m.winner = Entity{};
            }
            break;
        }
    }
}

usize select_spawn(scene::World& w, std::span<const math::Vec3> spawns,
                   Entity self) noexcept {
    if (spawns.empty()) return 0;

    usize best_idx = 0;
    f32 best_min_d2 = -1.0f;
    bool any_living = false;

    for (usize i = 0; i < spawns.size(); ++i) {
        const math::Vec3 sp = spawns[i];
        f32 min_d2 = std::numeric_limits<f32>::max();
        bool found = false;
        w.for_each_chunk_with_entities<scene::TransformWS, Health>(
            [&](usize n, const Entity* ents, scene::TransformWS* tw, Health* h) {
                for (usize k = 0; k < n; ++k) {
                    if (ents[k].raw == self.raw) continue;  // ignore the spawner
                    if (h[k].hp <= 0.0f) continue;          // dead don't threaten
                    const f32 dx = tw[k].mtw.m[12] - sp.x;
                    const f32 dy = tw[k].mtw.m[13] - sp.y;
                    const f32 dz = tw[k].mtw.m[14] - sp.z;
                    const f32 d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < min_d2) min_d2 = d2;
                    found = true;
                }
            });
        if (found) {
            any_living = true;
            // Strictly greater => the first (lowest-index) spawn wins ties.
            if (min_d2 > best_min_d2) {
                best_min_d2 = min_d2;
                best_idx = i;
            }
        }
    }

    return any_living ? best_idx : 0;  // no enemies alive -> first spawn
}

}  // namespace psynder::gameplay
