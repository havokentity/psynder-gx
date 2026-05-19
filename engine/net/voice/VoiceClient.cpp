// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/net/voice/VoiceClient.cpp
//
// Lane 19 — Opus voice-chat client implementation (Wave B scaffold).
//
// Implements PublicVoice.h:
//   init_client / shutdown_client — lifecycle, Opus encoder + decoder setup.
//   capture_frame                 — PCM in → Opus bytes out (20 ms frame).
//   mix_for_listener              — server-side mixing stub (M8 full impl).
//   mute / unmute / is_muted      — small flat hash-set of player IDs.
//
// Lane 14 (psy-audio) hook:
//   After decoding a received Opus packet the client feeds decoded PCM into
//   psynder::audio::feed_voice_pcm so the HRTF mixer can position the voice.
//   See engine/audio/VoiceMix.h for the full contract.
//
// -fno-fast-math is REQUIRED for Opus MDCT correctness; guaranteed by the
// project CompilerWarnings module (never add -ffast-math here or globally).

#include "net/voice/PublicVoice.h"
#include "audio/VoiceMix.h"
#include "core/Types.h"

// Opus C API
#include <opus.h>

#include <array>
#include <cstring>
#include <unordered_set>
#include <cmath>   // std::sqrtf, std::fmaf — must not be touched by fast-math

// ─── Module-private state ─────────────────────────────────────────────────

namespace {

// Opus frame parameters (matching VoiceDesc defaults).
constexpr int kSampleRate  = 48000;
constexpr int kChannels    = 1;        // mono capture; HRTF handles stereo
constexpr int kFrameSamples = 960;     // 20 ms @ 48 kHz
constexpr int kMaxPacket    = 4000;    // bytes — well above Opus max for 20ms

struct VoiceState {
    OpusEncoder* encoder  = nullptr;
    OpusDecoder* decoder  = nullptr;
    bool         active   = false;

    // Mute list: player IDs whose incoming voice is suppressed client-side.
    // Lane 18 (reliable channel) drives sync; lane 19 owns the local set.
    std::unordered_set<psynder::u32> muted_players;
};

VoiceState g_voice;

} // namespace

// ─── Public API ───────────────────────────────────────────────────────────

namespace psynder::net::voice {

void init_client(const VoiceDesc& desc) {
    if (g_voice.active) {
        // Already initialised — reinit cleanly.
        shutdown_client();
    }

    int err = 0;

    // Encoder: VOIP application preset for low latency + comfort-noise.
    g_voice.encoder = opus_encoder_create(
        kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);

    if (err != OPUS_OK || !g_voice.encoder) {
        // In a stub/early phase we do not abort; the rest of the engine
        // continues without voice capture.
        return;
    }

    // Set bitrate from VoiceDesc (kbps → bps).
    const int bitrate = static_cast<int>(desc.bitrate_kbps) * 1000;
    opus_encoder_ctl(g_voice.encoder, OPUS_SET_BITRATE(bitrate));

    // Enable DTX (discontinuous transmission) and in-band FEC for VAD mode.
    if (desc.capture == CaptureMode::Vad) {
        opus_encoder_ctl(g_voice.encoder, OPUS_SET_DTX(1));
        opus_encoder_ctl(g_voice.encoder, OPUS_SET_INBAND_FEC(1));
    }

    // Decoder: for local decode-to-playback path (received packets).
    g_voice.decoder = opus_decoder_create(kSampleRate, kChannels, &err);
    if (err != OPUS_OK || !g_voice.decoder) {
        opus_encoder_destroy(g_voice.encoder);
        g_voice.encoder = nullptr;
        return;
    }

    g_voice.active = true;
}

void shutdown_client() {
    if (g_voice.encoder) {
        opus_encoder_destroy(g_voice.encoder);
        g_voice.encoder = nullptr;
    }
    if (g_voice.decoder) {
        opus_decoder_destroy(g_voice.decoder);
        g_voice.decoder = nullptr;
    }
    g_voice.muted_players.clear();
    g_voice.active = false;
}

// Encode one 20ms PCM frame.
// pcm_in    — signed 16-bit mono, kFrameSamples samples expected.
// opus_out  — caller-supplied buffer; opus_out_cap must be >= kMaxPacket.
// Returns encoded packet length, or 0 on error / encoder not ready.
u32 capture_frame(std::int16_t* pcm_in, u32 pcm_samples,
                  std::uint8_t* opus_out, u32 opus_out_cap) {
    if (!g_voice.active || !g_voice.encoder) return 0u;
    if (!pcm_in || !opus_out)               return 0u;
    if (pcm_samples < static_cast<u32>(kFrameSamples)) return 0u;
    if (opus_out_cap < static_cast<u32>(kMaxPacket))   return 0u;

    const opus_int32 nbytes = opus_encode(
        g_voice.encoder,
        reinterpret_cast<const opus_int16*>(pcm_in),
        kFrameSamples,
        opus_out,
        static_cast<opus_int32>(opus_out_cap));

    if (nbytes < 0) return 0u;  // OPUS_* error code
    return static_cast<u32>(nbytes);
}

// ─── Server-side mixing stub ──────────────────────────────────────────────
//
// Full M8 implementation:
//   For each speaker in [speakers, speakers+n]:
//     1. Skip if speaker_id is in the listener's mute list.
//     2. Decode speaker->opus_data (Opus → float PCM, kFrameSamples).
//     3. Compute HRTF distance falloff:
//          float dx = speaker->world_pos[0] - listener_pos[0];
//          float dy = speaker->world_pos[1] - listener_pos[1];
//          float dz = speaker->world_pos[2] - listener_pos[2];
//          float dist = std::sqrt(dx*dx + dy*dy + dz*dz);  // metres
//          float gain  = 1.0f / std::fmax(1.0f, dist);     // inverse-distance
//     4. Mix scaled PCM into accumulator (float32 to avoid int overflow).
//     5. Soft-clip accumulator to [-1, 1].
//     6. Convert back to int16, re-encode to Opus → mixed_out.
//
// For now returns 0 (silence); the caller (dedicated server) must tolerate
// zero-byte output as "no packet this tick".

u32 mix_for_listener(const MixSpeaker* /*speakers*/, u32 /*n*/,
                     const float /*listener_pos*/[3],
                     std::uint8_t* /*mixed_out*/, u32 /*out_cap*/) {
    // TODO(lane-19, M8): decode + HRTF mix + re-encode.
    return 0u;
}

// ─── Mute / unmute ────────────────────────────────────────────────────────

void mute(u32 player_id) {
    g_voice.muted_players.insert(player_id);
}

void unmute(u32 player_id) {
    g_voice.muted_players.erase(player_id);
}

bool is_muted(u32 player_id) {
    return g_voice.muted_players.count(player_id) != 0u;
}

} // namespace psynder::net::voice

// ─── Lane 14 feed helper (internal, not part of PublicVoice.h) ───────────
//
// Called by the network-receive thread after decoding a remote player's
// Opus packet. Feeds decoded float PCM into psy-audio's voice ring buffer
// so the HRTF mixer can spatialise the voice.
//
// Place: same TU so the linker sees it without an extra header; callers in
// net/receive code include this TU's object via the psynder_net_voice static
// library. A forward-declare lives in net/voice/VoicePlayback.h (to be added
// in the full M8 pass alongside the receive-path glue).

namespace psynder::net::voice::internal {

// Decode an Opus packet and push PCM to lane 14's voice ring.
// Returns true if the channel was fed, false on mute / decode error.
bool decode_and_feed(audio::VoiceChannelId channel,
                     u32 speaker_id,
                     const u8*  opus_data,
                     u32        opus_size) {
    if (g_voice.muted_players.count(speaker_id) != 0u) return false;
    if (!g_voice.active || !g_voice.decoder)           return false;
    if (!opus_data || opus_size == 0)           return false;

    // Decode to float32 (Opus native output when using opus_decode_float).
    static f32 pcm_buf[kFrameSamples];

    const int decoded = opus_decode_float(
        g_voice.decoder,
        opus_data,
        static_cast<opus_int32>(opus_size),
        pcm_buf,
        kFrameSamples,
        /*decode_fec=*/0);

    if (decoded <= 0) return false;

    audio::feed_voice_pcm(channel, pcm_buf, static_cast<u32>(decoded));
    return true;
}

} // namespace psynder::net::voice::internal
