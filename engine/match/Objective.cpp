// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/Objective.cpp — see Objective.h. Pure capture-point state
// machine: single-occupant progress, full-capture ownership flip, contested/
// empty/owner-standing decay. Strict FP, no RNG, no alloc; deterministic.

#include "match/Objective.h"

namespace psynder::match {

void control_point_init(ControlPoint& cp) noexcept {
    cp.owner_team = kNoTeam;
    cp.capturing_team = kNoTeam;
    cp.progress = 0.0f;
}

u32 sole_occupant(std::span<const u32> team_counts) noexcept {
    u32 found = kNoTeam;
    for (usize t = 0; t < team_counts.size(); ++t) {
        if (team_counts[t] != 0u) {
            if (found != kNoTeam) return kNoTeam;  // a second team => contested
            found = static_cast<u32>(t);
        }
    }
    return found;  // kNoTeam when empty, else the only present team
}

bool is_contested(std::span<const u32> team_counts) noexcept {
    u32 present = 0u;
    for (usize t = 0; t < team_counts.size(); ++t) {
        if (team_counts[t] != 0u) {
            ++present;
            if (present >= 2u) return true;
        }
    }
    return false;
}

void tick_control_point(ControlPoint& cp, std::span<const u32> team_counts,
                        f32 capture_rate_per_s, f32 decay_rate_per_s,
                        f32 dt_s) noexcept {
    const u32 sole = sole_occupant(team_counts);

    // A real, non-owning sole occupant drives the capture forward.
    if (sole != kNoTeam && sole != cp.owner_team) {
        cp.capturing_team = sole;
        cp.progress += capture_rate_per_s * dt_s;
        if (cp.progress >= 1.0f) {
            // Capture complete: the point flips to the new owner and resets.
            cp.owner_team = sole;
            cp.progress = 0.0f;
            cp.capturing_team = kNoTeam;
            return;
        }
        // Guard against negative dt / rate leaving progress below 0.
        if (cp.progress < 0.0f) cp.progress = 0.0f;
        return;
    }

    // Contested, empty, or the owner standing on its own point: decay toward 0.
    cp.progress -= decay_rate_per_s * dt_s;
    if (cp.progress <= 0.0f) {
        cp.progress = 0.0f;
        cp.capturing_team = kNoTeam;
    } else if (cp.progress > 1.0f) {
        cp.progress = 1.0f;  // clamp (defensive against negative dt / rate)
    }
}

}  // namespace psynder::match
