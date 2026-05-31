// SPDX-License-Identifier: MIT
//
// engine/net/LossModel.cpp — deterministic loss/reorder model. See LossModel.h.

#include "net/LossModel.h"

namespace psynder::net {

namespace {

// splitmix64 (Vigna) — a fast, well-distributed integer finalizer. Pure: no
// state, same input => same output on every platform.
u64 splitmix64(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Independent hash streams for loss / reorder-fire / reorder-magnitude, so the
// three decisions are uncorrelated for the same (seq, seed).
constexpr u64 kSaltLoss = 0xA5A5A5A5A5A5A5A5ull;
constexpr u64 kSaltReorder = 0x3C3C3C3C3C3C3C3Cull;
constexpr u64 kSaltDelay = 0x1234567899887766ull;

u64 stream_hash(u64 seq, u64 seed, u64 salt) noexcept {
    // Mix the salt into the sequence, fold in the seed, then finalize.
    return splitmix64(splitmix64(seq + salt) ^ seed);
}

// Top 24 bits of a 64-bit hash -> a uniform f32 in [0, 1). 24 bits is exactly
// the f32 mantissa, so every representable value is hit and the result is exact.
f32 uniform01(u64 h) noexcept {
    return static_cast<f32>(h >> 40) * (1.0f / 16777216.0f);  // / 2^24
}

}  // namespace

bool drops(const LossConfig& cfg, u64 seq) noexcept {
    if (cfg.loss_rate <= 0.0f) return false;
    if (cfg.loss_rate >= 1.0f) return true;
    return uniform01(stream_hash(seq, cfg.seed, kSaltLoss)) < cfg.loss_rate;
}

u32 extra_delay_ticks(const LossConfig& cfg, u64 seq) noexcept {
    if (cfg.reorder_rate <= 0.0f || cfg.max_extra_delay_ticks == 0u) return 0u;
    if (uniform01(stream_hash(seq, cfg.seed, kSaltReorder)) >= cfg.reorder_rate) {
        return 0u;  // this packet keeps its order
    }
    // Fires: pick a delay in [1, max] from a separate magnitude stream.
    const u64 mag = stream_hash(seq, cfg.seed, kSaltDelay);
    return 1u + static_cast<u32>(mag % static_cast<u64>(cfg.max_extra_delay_ticks));
}

f32 measured_loss(const LossConfig& cfg, u64 first_seq, u64 count) noexcept {
    if (count == 0u) return 0.0f;
    u64 dropped = 0u;
    for (u64 i = 0; i < count; ++i) {
        if (drops(cfg, first_seq + i)) ++dropped;
    }
    return static_cast<f32>(dropped) / static_cast<f32>(count);
}

}  // namespace psynder::net
