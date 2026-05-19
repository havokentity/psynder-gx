// SPDX-License-Identifier: MIT
//
// engine/audio/VoiceMix.h
//
// Lane 14 — voice-channel mixing slot reserved for lane 19 (net-voice).
//
// Lane 19 Opus-decodes incoming voice PCM and pushes it here. The mixer
// thread reads it on the next pull cycle and treats it identically to any
// other in-game audio voice: positional HRTF, reverb send, soft-clip.
//
// API contract (FROZEN — public header):
//   open_voice_channel()  — called once per connected player on client.
//   close_voice_channel() — called on disconnect / mute-list add.
//   feed_voice_pcm()      — called per Opus decode callback (20 ms frames).
//
// Threading:
//   open/close must be called from the main/game thread.
//   feed_voice_pcm is safe to call from the network-receive thread; it
//   writes into a lock-free ring buffer sized for one Opus frame (960 samples
//   @ 48 kHz). If the ring is full the call is a no-op (mixer starving
//   is preferable to blocking the net thread).
//
// This TU is a stub: the ring buffer is a placeholder. The full
// implementation (ring + mixer integration) lands in M8 / lane-19 work.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

namespace psynder::audio {

// Opaque voice channel. Valid between open_voice_channel and
// close_voice_channel; do not store across those calls.
struct VoiceChannelTag {};
using VoiceChannelId = Handle<VoiceChannelTag>;

// Invalid sentinel (returned on failure, e.g. all 32 slots occupied).
inline constexpr VoiceChannelId kInvalidVoiceChannel{ 0u };

// Maximum concurrent voice channels (lane 19 enforces this at the net level
// too — 64-player match but not all talk at once; 32 is safe).
inline constexpr u32 kMaxVoiceChannels = 32u;

// PCM format expected by feed_voice_pcm: 48 kHz, mono, float32.
// Lane 19 is responsible for Opus decode to this format before calling in.
inline constexpr u32 kVoiceSampleRate   = 48000u;
inline constexpr u32 kVoiceChannelCount = 1u;    // mono; stereo out via HRTF

// ── Lifecycle ─────────────────────────────────────────────────────────────

// Reserve a voice channel for the given player. `world_pos` is the player's
// initial in-world position (used for HRTF / reverb; lane 19 updates it each
// tick via set_voice_position).
//
// Returns kInvalidVoiceChannel if all slots are occupied.
VoiceChannelId open_voice_channel(u32 player_id, math::Vec3 world_pos) noexcept;

// Release the channel (e.g. on disconnect or when the player is muted).
// Passing kInvalidVoiceChannel is a no-op.
void close_voice_channel(VoiceChannelId ch) noexcept;

// ── Per-frame update ──────────────────────────────────────────────────────

// Update the spatial position of a voice channel. Lane 19 calls this once
// per game tick with the player's current server-confirmed world position.
void set_voice_position(VoiceChannelId ch, math::Vec3 world_pos) noexcept;

// ── PCM feed (from network-receive / Opus-decode thread) ──────────────────

// Push mono float32 PCM decoded from an Opus frame into the channel's ring
// buffer. `pcm` must point to `samples` floats at kVoiceSampleRate.
//
// Standard Opus frame sizes: 120, 240, 480, or 960 samples (2.5, 5, 10,
// 20 ms). Lane 19 uses 960 (20 ms @ 48 kHz) for low overhead.
//
// Thread-safe (lock-free ring write). If the ring is full the call is
// silently dropped (the mixer generates comfort noise in the gap at M8).
void feed_voice_pcm(VoiceChannelId ch,
                    const f32*     pcm,
                    u32            samples) noexcept;

}  // namespace psynder::audio
