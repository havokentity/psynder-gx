// SPDX-License-Identifier: MIT
//
// engine/audio/VoiceMix.cpp
//
// Lane 14 — voice-channel mixing slot stub implementation.
//
// This TU wires the public VoiceMix.h API to per-slot state tracked in a
// fixed-size table. The ring-buffer PCM path and full mixer integration
// are deferred to M8 (DESIGN §10.9 + ADR-GX-007); what ships here is the
// complete slot lifecycle so lane 19 can call open/close/feed without
// undefined behaviour.
//
// Each VoiceChannelId packs a 24-bit slot index + 8-bit generation counter
// into a u32, identical to the VoicePool scheme in MixerCore.h. A stale
// id (close_voice_channel then re-open) produces a mismatched generation
// and is silently dropped, which is correct for the net-voice use case.

#include "VoiceMix.h"
#include "core/Log.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace psynder::audio {

namespace {

// ── Slot ──────────────────────────────────────────────────────────────────
struct VoiceChannelSlot {
    std::atomic<bool> active{false};
    u32               gen      = 0u;   // generation counter
    u32               player_id = 0u;
    math::Vec3        world_pos{0.0f, 0.0f, 0.0f};

    // Placeholder ring-buffer: a single 960-sample mono float32 frame.
    // M8 will replace this with a proper lock-free ring of multiple frames.
    static constexpr u32 kRingFrames = 960u;
    f32                  ring[kRingFrames]{};
    std::atomic<u32>     ring_write_pos{0u};  // mod kRingFrames, lock-free
};

std::array<VoiceChannelSlot, kMaxVoiceChannels> g_slots{};
std::mutex                                      g_slot_mu{};

// Pack/unpack matching VoicePool in MixerCore.h
inline VoiceChannelId pack_id(u32 idx, u32 gen) noexcept {
    return VoiceChannelId{ (idx & 0x00FFFFFFu) | ((gen & 0xFFu) << 24) };
}
inline u32 unpack_idx(VoiceChannelId id) noexcept { return id.raw & 0x00FFFFFFu; }
inline u32 unpack_gen(VoiceChannelId id) noexcept { return (id.raw >> 24) & 0xFFu; }

inline bool valid_id(VoiceChannelId id) noexcept {
    if (id.raw == 0u) return false;
    const u32 idx = unpack_idx(id);
    if (idx >= kMaxVoiceChannels) return false;
    const auto& slot = g_slots[idx];
    return slot.active.load(std::memory_order_acquire) &&
           slot.gen == unpack_gen(id);
}

}  // namespace

// ── Lifecycle ──────────────────────────────────────────────────────────────

VoiceChannelId open_voice_channel(u32 player_id, math::Vec3 world_pos) noexcept {
    std::lock_guard<std::mutex> lk(g_slot_mu);
    for (u32 i = 0; i < kMaxVoiceChannels; ++i) {
        VoiceChannelSlot& s = g_slots[i];
        if (!s.active.load(std::memory_order_relaxed)) {
            // bump gen (never zero so packed id != kInvalidVoiceChannel)
            s.gen = (s.gen + 1u) & 0xFFu;
            if (s.gen == 0u) s.gen = 1u;
            s.player_id = player_id;
            s.world_pos = world_pos;
            std::memset(s.ring, 0, sizeof(s.ring));
            s.ring_write_pos.store(0u, std::memory_order_relaxed);
            s.active.store(true, std::memory_order_release);
            PSY_LOG_INFO("[audio] voice channel opened: player={} slot={}", player_id, i);
            return pack_id(i, s.gen);
        }
    }
    PSY_LOG_WARN("[audio] open_voice_channel: all {} slots occupied", kMaxVoiceChannels);
    return kInvalidVoiceChannel;
}

void close_voice_channel(VoiceChannelId ch) noexcept {
    if (ch.raw == 0u) return;
    const u32 idx = unpack_idx(ch);
    if (idx >= kMaxVoiceChannels) return;
    VoiceChannelSlot& s = g_slots[idx];
    // Validate generation before closing (stale id = no-op).
    {
        std::lock_guard<std::mutex> lk(g_slot_mu);
        if (!s.active.load(std::memory_order_relaxed)) return;
        if (s.gen != unpack_gen(ch)) return;
        s.active.store(false, std::memory_order_release);
    }
    PSY_LOG_INFO("[audio] voice channel closed: slot={}", idx);
}

// ── Per-frame update ────────────────────────────────────────────────────────

void set_voice_position(VoiceChannelId ch, math::Vec3 world_pos) noexcept {
    if (!valid_id(ch)) return;
    g_slots[unpack_idx(ch)].world_pos = world_pos;
}

// ── PCM feed ────────────────────────────────────────────────────────────────

void feed_voice_pcm(VoiceChannelId ch, const f32* pcm, u32 samples) noexcept {
    if (!valid_id(ch) || !pcm || samples == 0u) return;
    VoiceChannelSlot& s = g_slots[unpack_idx(ch)];
    // Stub: write into the ring with simple wraparound; no blocking.
    // M8 will replace with a true lock-free producer/consumer ring with
    // mixer thread integration.
    const u32 cap   = VoiceChannelSlot::kRingFrames;
    u32       pos   = s.ring_write_pos.load(std::memory_order_relaxed);
    const u32 to_write = (samples < cap) ? samples : cap;
    for (u32 i = 0; i < to_write; ++i) {
        s.ring[pos] = pcm[i];
        pos = (pos + 1u) % cap;
    }
    s.ring_write_pos.store(pos, std::memory_order_release);
}

}  // namespace psynder::audio
