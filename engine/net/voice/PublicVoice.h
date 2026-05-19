// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/net/voice/PublicVoice.h
//
// Lane 19 — Voice chat public-header CONTRACT (Wave 0).
//
// Opus codec, server-side mixing per recipient, positional voice through
// shared psy-audio HRTF, push-to-talk + open-mic + VAD.
//
// See DESIGN-PSYNDER-GX.md §10.9.

#pragma once

#include <cstdint>

namespace psynder::net::voice {

enum class CaptureMode : std::uint8_t {
    PushToTalk,
    OpenMic,
    Vad,        // voice-activation detection
};

struct VoiceDesc {
    std::uint32_t sample_rate_hz = 48000; // Opus native
    std::uint32_t frame_ms       = 20;    // 20ms Opus frame
    std::uint32_t bitrate_kbps   = 24;    // server-mixed result is ~sub-2KB/tick per client
    CaptureMode   capture        = CaptureMode::PushToTalk;
    bool          positional     = true;  // route through psy-audio HRTF
};

// Lifecycle (client side).
void init_client(const VoiceDesc&);
void shutdown_client();

// Mic capture: push raw PCM in, get an Opus-encoded packet out.
// Returns the packet size on success, 0 if no packet ready this tick.
std::uint32_t capture_frame(std::int16_t* pcm_in, std::uint32_t pcm_samples,
                            std::uint8_t* opus_out, std::uint32_t opus_out_cap);

// Server-side mix: takes opus packets from N speakers + the listener's
// world position; produces one pre-mixed Opus packet for the listener.
// O(speakers_in_range) per listener per tick.
struct MixSpeaker {
    std::uint32_t speaker_id;
    const std::uint8_t* opus_data;
    std::uint32_t       opus_size;
    float               world_pos[3];
};
std::uint32_t mix_for_listener(const MixSpeaker* speakers, std::uint32_t n,
                               const float listener_pos[3],
                               std::uint8_t* mixed_out, std::uint32_t out_cap);

// Mute lists synced via reliable channel (lane 18).
void mute(std::uint32_t player_id);
void unmute(std::uint32_t player_id);
bool is_muted(std::uint32_t player_id);

} // namespace psynder::net::voice
