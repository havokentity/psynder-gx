// SPDX-License-Identifier: MIT
// Psynder-GX — a DETERMINISTIC packet loss / reorder model. Lane 18 (net).
//
// A reproducible test channel for exercising netcode robustness: given a packet
// sequence number it decides — purely from a hash of (seq, seed) — whether the
// packet is LOST and what deterministic extra DELAY (for reorder simulation) it
// suffers. There is NO RNG state: the same (seq, config) ALWAYS yields the same
// decision, so a test run is perfectly reproducible across runs and platforms
// (unlike std::rand). Drop the channel in front of Loopback/Reliability in a
// test to simulate a lossy/jittery link without flaky nondeterminism.
//
// Pure integer hashing (a local splitmix64) + an f32 compare — strict-FP net
// lane, no transcendentals, no mutable state, no allocation.

#pragma once

#include "core/Types.h"

namespace psynder::net {

// loss_rate / reorder_rate are probabilities in [0,1]; max_extra_delay_ticks is
// the largest reorder delay (in ticks); seed selects the impairment pattern.
struct LossConfig {
    f32 loss_rate = 0.0f;
    f32 reorder_rate = 0.0f;
    u32 max_extra_delay_ticks = 0u;
    u64 seed = 0u;
};

// True iff packet `seq` is dropped: a splitmix64 hash of (seq, seed) mapped to a
// uniform f32 in [0,1) compared < loss_rate. loss_rate <= 0 never drops; >= 1
// always drops. Pure + reproducible.
bool drops(const LossConfig& cfg, u64 seq) noexcept;

// Deterministic extra arrival delay (ticks) for packet `seq`, for reorder
// simulation: 0 unless this packet's INDEPENDENT reorder hash fires (with
// probability reorder_rate), in which case a delay in [1, max_extra_delay_ticks].
// Independent of `drops` (a dropped packet still has a well-defined delay value),
// so loss and reorder are uncorrelated streams. 0 when reorder_rate <= 0 or
// max_extra_delay_ticks == 0.
u32 extra_delay_ticks(const LossConfig& cfg, u64 seq) noexcept;

// The empirical fraction dropped over `count` consecutive sequence numbers
// starting at `first_seq` — lets a test assert the model's measured loss tracks
// loss_rate over a large sample. Returns 0 for count == 0.
f32 measured_loss(const LossConfig& cfg, u64 first_seq, u64 count) noexcept;

}  // namespace psynder::net
