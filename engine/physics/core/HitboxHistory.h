// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/HitboxHistory.h
//
// Lane 15 (physics-core) — ADR-020 / issue #42: lag compensation, a.k.a.
// hitbox "backtracking" / rewind. SCAFFOLD ONLY (Wave 0): a fixed-capacity
// ring buffer of per-tick hitbox poses with a record / rewind contract.
//
// Why: an authoritative server validates a client's hitscan against where the
// targets WERE on the tick the shooter actually fired (their estimated server
// time), not where they ARE now after the round-trip latency. We keep a short
// rolling history of every body's hitbox AABB, keyed by the fixed 120 Hz
// physics tick (DESIGN §10.1, §14). On a fire event the server rewinds the
// hitboxes to the firing tick, runs the ray test against those poses, then
// restores the present.
//
// Determinism + DOTS contract (AGENTS.md / DESIGN §3, §14):
//   * POD / trivially-copyable poses, SoA-friendly fixed-extent storage.
//   * No heap allocation after construction — the ring is sized at compile
//     time and lives inline. No exceptions / RTTI on the hot path.
//   * Tick-indexed, never wall-clock. Rewind resolves to the newest recorded
//     tick at or before the requested tick (stable, replay-safe).
//
// TODO(ADR-020/#42): integrate with the Jolt body set + CharacterSpine so the
//   server records every authoritative hitbox each tick and the hitscan path
//   (samples/combat) queries against a rewound snapshot. Multi-body-per-tick
//   layout, per-bone hitboxes, and interpolation between ticks are follow-ups.

#pragma once

#include "core/Types.h"

#include <array>
#include <span>
#include <type_traits>

namespace psynder::physics {

// A single axis-aligned hitbox pose for one tick. Metric: 1 unit = 1 metre.
// POD by design — `center` is the AABB centre in world space, `half_extents`
// the half-widths on each axis.
struct HitboxPose {
    f32 center[3]{};
    f32 half_extents[3]{};
};
static_assert(std::is_trivially_copyable_v<HitboxPose>,
              "HitboxPose must stay POD for the DOTS / determinism contract");

// Fixed-capacity ring buffer of per-tick hitbox snapshots.
//
//   * Capacity = number of distinct ticks retained (the rewind window).
//   * MaxPosesPerTick = upper bound on hitboxes recorded for one tick.
//
// All storage is inline; nothing allocates after construction. At a 120 Hz
// tick a Capacity of 128 covers ~1.07 s of rewind, comfortably beyond any
// reasonable RTT + interpolation budget.
template <psynder::u32 Capacity = 128, psynder::u32 MaxPosesPerTick = 64>
class HitboxHistory {
    static_assert(Capacity > 0, "rewind window must hold at least one tick");
    static_assert(MaxPosesPerTick > 0, "must retain at least one pose per tick");

public:
    static constexpr psynder::u32 capacity        = Capacity;
    static constexpr psynder::u32 max_poses        = MaxPosesPerTick;

    HitboxHistory() = default;

    // Record the hitbox poses for `tick`. Poses beyond MaxPosesPerTick are
    // dropped (bounded memory). Recording overwrites the oldest slot once the
    // window is full. Ticks are expected to arrive monotonically (the fixed
    // simulation tick), but out-of-order writes are tolerated — rewind always
    // scans for the best match.
    void record(psynder::u32 tick, std::span<const HitboxPose> poses) noexcept {
        const psynder::u32 slot = count_ < Capacity ? count_ : oldest_;
        Frame& f = frames_[slot];
        f.tick  = tick;
        f.valid = true;
        const psynder::u32 n =
            static_cast<psynder::u32>(poses.size()) < MaxPosesPerTick
                ? static_cast<psynder::u32>(poses.size())
                : MaxPosesPerTick;
        f.count = n;
        for (psynder::u32 i = 0; i < n; ++i) f.poses[i] = poses[i];

        if (count_ < Capacity) {
            ++count_;
        } else {
            oldest_ = (oldest_ + 1) % Capacity;
        }
    }

    // Rewind: return the poses recorded at the newest tick that is <= `tick`.
    // Empty span if nothing recorded at or before `tick` (or no history yet).
    // Deterministic: a stable linear scan over the fixed-extent ring.
    std::span<const HitboxPose> rewind(psynder::u32 tick) const noexcept {
        const Frame* best = nullptr;
        for (psynder::u32 i = 0; i < count_; ++i) {
            const Frame& f = frames_[i];
            if (!f.valid || f.tick > tick) continue;
            if (best == nullptr || f.tick > best->tick) best = &f;
        }
        if (best == nullptr) return {};
        return std::span<const HitboxPose>(best->poses.data(), best->count);
    }

    // Number of distinct ticks currently retained.
    psynder::u32 size() const noexcept { return count_; }
    bool         empty() const noexcept { return count_ == 0; }

private:
    struct Frame {
        std::array<HitboxPose, MaxPosesPerTick> poses{};
        psynder::u32 tick  = 0;
        psynder::u32 count = 0;
        bool         valid = false;
    };

    std::array<Frame, Capacity> frames_{};
    psynder::u32 count_  = 0;  // distinct ticks recorded (<= Capacity)
    psynder::u32 oldest_ = 0;  // ring head once full
};

}  // namespace psynder::physics
