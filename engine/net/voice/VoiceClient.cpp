// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/net/voice/VoiceClient.cpp
//
// Lane 19 — Opus voice-chat client + server-side mixer implementation.
//
// Implements PublicVoice.h:
//   init_client / shutdown_client      — lifecycle, Opus encoder + decoder setup.
//   init_server / shutdown_server      — server-side per-listener encoder pool.
//   capture_frame                      — PCM in → Opus bytes out (20 ms frame)
//                                        with energy-gate VAD.
//   mix_for_listener                   — full server-side positional Opus mix.
//   mute / unmute / is_muted           — small flat hash-set of player IDs.
//
// Lane 14 (psy-audio) hook:
//   After decoding a received Opus packet the client feeds decoded PCM into
//   psynder::audio::feed_voice_pcm so the HRTF mixer can position the voice.
//   See engine/audio/VoiceMix.h for the full contract.
//
// Metric units: 1 world unit = 1 metre (per DESIGN §14).
// -fno-fast-math is REQUIRED for Opus MDCT correctness; guaranteed by the
// project CompilerWarnings module (never add -ffast-math here or globally).

#include "net/voice/PublicVoice.h"
#include "audio/VoiceMix.h"
#include "core/Types.h"

// Opus C API
#include <opus.h>

#include <array>
#include <cmath>       // std::sqrt, std::tanh — must not be touched by fast-math
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─── Module-private constants ─────────────────────────────────────────────

namespace {

// Opus frame parameters (matching VoiceDesc defaults).
constexpr int    kSampleRate   = 48000;
constexpr int    kChannels     = 1;        // mono capture; HRTF handles stereo
constexpr int    kFrameSamples = 960;      // 20 ms @ 48 kHz
constexpr int    kMaxPacket    = 4000;     // bytes — well above Opus max for 20ms

// Distance rolloff (inverse-distance, clamped to plateau at kNearClip).
// 1 world unit = 1 metre (DESIGN §14). Below kNearClip gain is 1/kNearClip,
// i.e. the gain plateaus instead of rising to infinity at zero distance.
// Inverse-distance (not inverse-square) is the standard perceptual rolloff
// for voice — see DESIGN §10.9 and the comment at mix_for_listener step 2.
constexpr float  kNearClip     = 0.5f;    // metres — avoids gain→∞ singularity

// VAD energy gate: -50 dBFS short-term RMS gate with hysteresis.
//   open_threshold  = 10^(-50/20)  ≈ 0.003162 (linear amplitude RMS)
//   close_threshold = 10^(-55/20)  ≈ 0.001778 (a few dB below open to avoid chatter)
// These are *RMS of the float [-1,1] signal*, so energy = mean(x²),
// rms = sqrt(energy); gate opens when rms >= kVadOpen, closes when rms <= kVadClose.
constexpr float  kVadOpen      = 0.003162f;
constexpr float  kVadClose     = 0.001778f;

// ─── Client state ─────────────────────────────────────────────────────────

struct VoiceState {
    OpusEncoder* encoder  = nullptr;
    OpusDecoder* decoder  = nullptr;  // single-stream client-side rx decoder
    bool         active   = false;
    bool         vad_open = false;    // hysteresis state for VAD gate

    // Configured capture mode (PushToTalk / OpenMic / Vad). The VAD energy
    // gate only fires when capture == Vad — PushToTalk and OpenMic emit
    // every frame the caller hands us. Retained from VoiceDesc on init.
    psynder::net::voice::CaptureMode capture =
        psynder::net::voice::CaptureMode::PushToTalk;

    // Mute list: player IDs whose incoming voice is suppressed client-side.
    // Lane 18 (reliable channel) drives sync; lane 19 owns the local set.
    // Used ONLY by is_muted() and the receive-side decode-to-playback path;
    // server-side mix_for_listener uses MixSpeaker::muted_by_listener instead.
    std::unordered_set<psynder::u32> muted_players;
};

VoiceState g_voice;

// ─── Server state ─────────────────────────────────────────────────────────
//
// The server holds:
//   - Per-listener OpusEncoder*  (re-encode mixed output → Opus packet)
//   - Per-speaker  OpusDecoder*  (decode each speaker's Opus packet)
//
// Encoders are indexed 0..max_listeners-1 (opaque slot, managed by the
// caller who maps listener slot → speaker stream).
//
// Decoders are keyed by speaker_id so the same decoder object is reused
// across ticks, preserving Opus codec state (important for continuity).

struct ServerState {
    std::vector<OpusEncoder*>               listener_encoders;
    std::unordered_map<psynder::u32, OpusDecoder*> speaker_decoders;
    std::uint32_t                           max_listeners = 0;
    bool                                    active        = false;
};

ServerState g_server;

} // namespace (anonymous)

// ─── Public API ───────────────────────────────────────────────────────────

namespace psynder::net::voice {

// ── Client lifecycle ──────────────────────────────────────────────────────

void init_client(const VoiceDesc& desc) {
    if (g_voice.active) {
        shutdown_client();
    }

    int err = 0;

    // Encoder: VOIP application preset for low latency + comfort-noise.
    g_voice.encoder = opus_encoder_create(
        kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);

    if (err != OPUS_OK || !g_voice.encoder) {
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

    g_voice.vad_open = false;
    g_voice.capture  = desc.capture;
    g_voice.active   = true;
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
    g_voice.vad_open = false;
    g_voice.capture  = psynder::net::voice::CaptureMode::PushToTalk;
    g_voice.active   = false;
}

// ── Server lifecycle ──────────────────────────────────────────────────────
//
// shutdown_server defined first so init_server may call it on reinit.

void shutdown_server() {
    for (OpusEncoder* enc : g_server.listener_encoders) {
        if (enc) opus_encoder_destroy(enc);
    }
    g_server.listener_encoders.clear();

    for (auto& [id, dec] : g_server.speaker_decoders) {
        if (dec) opus_decoder_destroy(dec);
    }
    g_server.speaker_decoders.clear();

    g_server.max_listeners = 0;
    g_server.active        = false;
}

// Call once from the dedicated-server startup path.
// max_listeners: maximum simultaneous voice listeners (e.g. 64 for 64-player).
// Allocates one OpusEncoder per listener slot; speaker decoders are lazily
// allocated on first packet and reused each tick.

void init_server(u32 max_listeners) {
    if (g_server.active) {
        shutdown_server();
    }

    g_server.max_listeners = max_listeners;
    g_server.listener_encoders.resize(max_listeners, nullptr);

    for (u32 i = 0; i < max_listeners; ++i) {
        int err = 0;
        OpusEncoder* enc = opus_encoder_create(
            kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
        if (err == OPUS_OK && enc) {
            // 24 kbps — sub-2 KB per 20ms frame.
            opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
            g_server.listener_encoders[i] = enc;
        }
    }

    g_server.active = true;
}

// ── Mic capture (client side) ─────────────────────────────────────────────
//
// pcm_in    — signed 16-bit mono, kFrameSamples samples expected.
// opus_out  — caller-supplied buffer; opus_out_cap must be >= kMaxPacket.
// Returns encoded packet length, or 0 on error / encoder not ready / VAD gated.
//
// VAD gate (when g_voice.active && CaptureMode has energy check):
//   Compute short-term RMS of the input frame.
//   If rms >= kVadOpen  → gate opens,  frame is encoded + returned.
//   If rms <= kVadClose → gate closes, return 0 (suppress silence).
//   Hysteresis prevents rapid on/off flutter around the threshold.

u32 capture_frame(std::int16_t* pcm_in, u32 pcm_samples,
                  std::uint8_t* opus_out, u32 opus_out_cap) {
    if (!g_voice.active || !g_voice.encoder) return 0u;
    if (!pcm_in || !opus_out)               return 0u;
    if (pcm_samples < static_cast<u32>(kFrameSamples)) return 0u;
    if (opus_out_cap < static_cast<u32>(kMaxPacket))   return 0u;

    // ── VAD energy gate (only in CaptureMode::Vad) ────────────────────────
    // PushToTalk and OpenMic emit every frame the caller hands us — the
    // PTT key state (PTT) or the always-on policy (OpenMic) decides upstream
    // whether capture_frame() is called this tick. Only Vad inspects the
    // signal and may return 0 for quiet frames.
    if (g_voice.capture == CaptureMode::Vad) {
        // Compute energy of the int16 frame as float RMS.
        // Scale: int16 full scale = 32768, so normalise to [-1, 1] range.
        double energy = 0.0;
        const double inv_scale = 1.0 / 32768.0;
        for (u32 i = 0; i < static_cast<u32>(kFrameSamples); ++i) {
            const double s = static_cast<double>(pcm_in[i]) * inv_scale;
            energy += s * s;
        }
        const double rms = std::sqrt(energy / static_cast<double>(kFrameSamples));

        if (g_voice.vad_open) {
            // Gate is open — close only when rms drops below kVadClose.
            if (rms < static_cast<double>(kVadClose)) {
                g_voice.vad_open = false;
                return 0u;  // suppress silence
            }
        } else {
            // Gate is closed — open when rms rises above kVadOpen.
            if (rms < static_cast<double>(kVadOpen)) {
                return 0u;  // below threshold, stay closed
            }
            g_voice.vad_open = true;
        }
    }

    const opus_int32 nbytes = opus_encode(
        g_voice.encoder,
        reinterpret_cast<const opus_int16*>(pcm_in),
        kFrameSamples,
        opus_out,
        static_cast<opus_int32>(opus_out_cap));

    if (nbytes < 0) return 0u;
    return static_cast<u32>(nbytes);
}

// ── Server-side mixing ────────────────────────────────────────────────────
//
// Produces one pre-mixed Opus packet for the listener at listener_pos from
// the N speakers in [speakers, speakers+n].
//
// Algorithm:
//   1. Skip speakers muted by the listener's mute list.
//   2. Compute inverse-distance gain (metric, 1 WU = 1 m) with near-clip
//      plateau at kNearClip to avoid singularity.
//   3. Decode each speaker's Opus packet to float PCM (kFrameSamples).
//      Speaker decoders are lazily created keyed by speaker_id and reused.
//   4. Multiply decoded PCM by gain and accumulate into float mix buffer.
//   5. After all speakers: soft-clip via tanh(x * k) / k (k = 1.5). This
//      preserves small signals near-linearly (the output is ≈ x while
//      |x| << 1) and gently saturates large peaks. Note the asymptote of
//      tanh(x*k)/k is ±1/k = ±0.667 (NOT ±1) — by design we leave 3.5 dB
//      of headroom so the Opus encoder's own dynamics stay clean.
//   6. Re-encode mix buffer to Opus → mixed_out.
//
// Returns the byte count of the output Opus packet, or 0 if no audio.

u32 mix_for_listener(u32 listener_slot,
                     const MixSpeaker* speakers, u32 n,
                     const float listener_pos[3],
                     std::uint8_t* mixed_out, u32 out_cap) {
    if (!speakers || n == 0)  return 0u;
    if (!mixed_out)           return 0u;
    if (out_cap < static_cast<u32>(kMaxPacket)) return 0u;

    // Float accumulator for the mix — 960 samples, zero-init.
    static float mix_buf[kFrameSamples];
    std::memset(mix_buf, 0, sizeof(mix_buf));

    // Temp decode buffer (re-used across speakers).
    static float decode_buf[kFrameSamples];

    bool any_audio = false;

    for (u32 s = 0; s < n; ++s) {
        const MixSpeaker& spk = speakers[s];

        // 1. Skip speakers the listener has muted.  The caller pre-fills
        //    `muted_by_listener` from this listener's mute list — we do NOT
        //    consult g_voice.muted_players here because that set is the
        //    *local client's* mute list, which is meaningless for a server
        //    mixing audio for many listeners (Copilot review on PR #6).
        if (spk.muted_by_listener) continue;
        if (!spk.opus_data || spk.opus_size == 0) continue;

        // 2. Inverse-distance gain (metric). Plateau at kNearClip.
        const float dx   = spk.world_pos[0] - listener_pos[0];
        const float dy   = spk.world_pos[1] - listener_pos[1];
        const float dz   = spk.world_pos[2] - listener_pos[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);  // metres
        // gain = 1 / max(kNearClip, dist); inverse-distance (not square) is the
        // standard perceptual rolloff for voice (DESIGN §10.9).
        const float safe_dist = (dist < kNearClip) ? kNearClip : dist;
        const float gain      = 1.0f / safe_dist;

        // 3. Decode speaker's Opus packet to float PCM.
        // Lazily create the decoder keyed by speaker_id; reuse across ticks
        // to preserve codec continuity (critical for Opus MDCT overlap state).
        OpusDecoder*& dec_ref = g_server.speaker_decoders[spk.speaker_id];
        if (!dec_ref) {
            int err = 0;
            dec_ref = opus_decoder_create(kSampleRate, kChannels, &err);
            if (err != OPUS_OK || !dec_ref) {
                g_server.speaker_decoders.erase(spk.speaker_id);
                continue;
            }
        }

        const int decoded = opus_decode_float(
            dec_ref,
            spk.opus_data,
            static_cast<opus_int32>(spk.opus_size),
            decode_buf,
            kFrameSamples,
            /*decode_fec=*/0);

        if (decoded <= 0) continue;

        // 4. Accumulate into mix buffer with gain.
        const int count = (decoded < kFrameSamples) ? decoded : kFrameSamples;
        for (int i = 0; i < count; ++i) {
            mix_buf[i] += decode_buf[i] * gain;
        }
        any_audio = true;
    }

    if (!any_audio) return 0u;

    // 5. Soft-clip: tanh(x * k) / k where k = 1.5.
    // This maps x in [-1, 1] to approx x (linear), then gently saturates
    // beyond ±1 instead of clipping hard. Preserves dynamics at normal levels.
    {
        constexpr float k    = 1.5f;
        constexpr float inv_k = 1.0f / k;
        for (int i = 0; i < kFrameSamples; ++i) {
            mix_buf[i] = std::tanh(mix_buf[i] * k) * inv_k;
        }
    }

    // 6. Re-encode to Opus. Each listener owns a stream-stable encoder slot
    // (init_server preallocates max_listeners encoders). Stream-stability
    // preserves Opus codec state — MDCT overlap, DTX/CNG, LPC history —
    // across ticks for that listener's outbound stream, which is essential
    // for continuity and DTX correctness.
    //
    // Fallback paths: an out-of-range listener_slot, an inactive server
    // (typical for unit tests driving mix_for_listener standalone), or a
    // null encoder in that slot all route to a one-shot temporary encoder.
    OpusEncoder* enc = nullptr;
    if (g_server.active && listener_slot < g_server.listener_encoders.size()) {
        enc = g_server.listener_encoders[listener_slot];
    }

    // Fallback: create a temporary encoder (unit-test path or unconfigured server).
    OpusEncoder* temp_enc = nullptr;
    if (!enc) {
        int err = 0;
        temp_enc = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
        if (err != OPUS_OK || !temp_enc) return 0u;
        opus_encoder_ctl(temp_enc, OPUS_SET_BITRATE(24000));
        enc = temp_enc;
    }

    const opus_int32 nbytes = opus_encode_float(
        enc,
        mix_buf,
        kFrameSamples,
        mixed_out,
        static_cast<opus_int32>(out_cap));

    if (temp_enc) opus_encoder_destroy(temp_enc);

    if (nbytes < 0) return 0u;
    return static_cast<u32>(nbytes);
}

// ── Mute list ─────────────────────────────────────────────────────────────

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

namespace psynder::net::voice::internal {

bool decode_and_feed(audio::VoiceChannelId channel,
                     u32 speaker_id,
                     const u8*  opus_data,
                     u32        opus_size) {
    if (g_voice.muted_players.count(speaker_id) != 0u) return false;
    if (!g_voice.active || !g_voice.decoder)           return false;
    if (!opus_data || opus_size == 0)                  return false;

    // Decode to float32.
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
