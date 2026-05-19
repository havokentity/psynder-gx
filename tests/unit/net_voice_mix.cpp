// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/net_voice_mix.cpp
//
// Lane 19 — unit tests for server-side Opus mixing.
//
// Tests:
//   1. mix_for_listener with 2 speakers both muted → output decodes to silence.
//   2. mix_for_listener with 2 equidistant unmuted speakers emitting tones
//      → output decodes to non-silence (sum of tones present).
//   3. VAD gate: energy above kVadOpen threshold → packet produced.
//      Energy below kVadClose threshold → 0 returned (silence suppressed).
//
// Topology: 2 fake speakers, 1 listener.
// Speakers are equidistant at exactly 1 m from the listener (gain = 1.0
// at kNearClip = 0.5 m plateau, so gain = 1/1 = 1.0 for dist ≥ 1 m).
//
// -fno-fast-math is inherited from the project flags on this TU via linking
// against psynder_net_voice (which carries -fno-fast-math on its target).

#include <catch2/catch_test_macros.hpp>

#include "net/voice/PublicVoice.h"
#include "net/voice/VoiceServer_internal.h"

#include <opus.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// ── Helpers ──────────────────────────────────────────────────────────────

constexpr int    kRate        = 48000;
constexpr int    kFrameSamples = 960;          // 20 ms
constexpr float  kTwoPI       = 6.28318530718f;

// Generate a mono float32 sine tone at `freq_hz` into `out[kFrameSamples]`.
void make_sine_float(float* out, float freq_hz, float amplitude = 0.5f) {
    for (int i = 0; i < kFrameSamples; ++i) {
        out[i] = amplitude * std::sin(kTwoPI * freq_hz * static_cast<float>(i)
                                       / static_cast<float>(kRate));
    }
}

// Generate a mono int16 sine tone at `freq_hz` into `out[kFrameSamples]`.
void make_sine_i16(std::int16_t* out, float freq_hz, float amplitude = 0.4f) {
    for (int i = 0; i < kFrameSamples; ++i) {
        float s = amplitude * std::sin(kTwoPI * freq_hz * static_cast<float>(i)
                                        / static_cast<float>(kRate));
        out[i] = static_cast<std::int16_t>(s * 32767.0f);
    }
}

// Encode a float PCM buffer to Opus. Returns bytes written.
std::uint32_t encode_float(OpusEncoder* enc, const float* pcm,
                            std::uint8_t* out, std::uint32_t cap) {
    const opus_int32 n = opus_encode_float(enc, pcm, kFrameSamples,
                                           out, static_cast<opus_int32>(cap));
    return (n < 0) ? 0u : static_cast<std::uint32_t>(n);
}

// Decode an Opus packet to float PCM. Returns sample count.
int decode_float(OpusDecoder* dec, const std::uint8_t* pkt, std::uint32_t sz,
                 float* out) {
    return opus_decode_float(dec, pkt, static_cast<opus_int32>(sz),
                              out, kFrameSamples, 0);
}

// RMS energy of a float buffer.
double rms(const float* buf, int n) {
    double e = 0.0;
    for (int i = 0; i < n; ++i) e += static_cast<double>(buf[i]) * buf[i];
    return std::sqrt(e / static_cast<double>(n));
}

} // namespace (anonymous)

// ─────────────────────────────────────────────────────────────────────────
// TEST 1: Both speakers muted → listener output decodes to silence
// ─────────────────────────────────────────────────────────────────────────
TEST_CASE("net-voice: muted speakers produce silence at listener",
          "[net][voice][mix]") {

    psynder::net::voice::init_client({});

    // Build two Opus packets from sine tones (440 Hz and 880 Hz).
    int err = 0;
    OpusEncoder* enc1 = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    REQUIRE(err == OPUS_OK);
    OpusEncoder* enc2 = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    REQUIRE(err == OPUS_OK);

    float pcm1[kFrameSamples], pcm2[kFrameSamples];
    make_sine_float(pcm1, 440.0f);
    make_sine_float(pcm2, 880.0f);

    std::array<std::uint8_t, 4000> pkt1_buf, pkt2_buf;
    const auto sz1 = encode_float(enc1, pcm1, pkt1_buf.data(), 4000);
    const auto sz2 = encode_float(enc2, pcm2, pkt2_buf.data(), 4000);
    REQUIRE(sz1 > 0);
    REQUIRE(sz2 > 0);

    opus_encoder_destroy(enc1);
    opus_encoder_destroy(enc2);

    // Listener sits at origin; speakers at (1, 0, 0) and (-1, 0, 0).
    const float listener_pos[3] = {0.0f, 0.0f, 0.0f};

    psynder::net::voice::MixSpeaker speakers[2] = {};
    speakers[0].speaker_id = 1;
    speakers[0].opus_data  = pkt1_buf.data();
    speakers[0].opus_size  = sz1;
    speakers[0].world_pos[0] = 1.0f; speakers[0].world_pos[1] = 0.0f; speakers[0].world_pos[2] = 0.0f;

    speakers[1].speaker_id = 2;
    speakers[1].opus_data  = pkt2_buf.data();
    speakers[1].opus_size  = sz2;
    speakers[1].world_pos[0] = -1.0f; speakers[1].world_pos[1] = 0.0f; speakers[1].world_pos[2] = 0.0f;

    // Mute both speakers from listener's perspective.
    psynder::net::voice::mute(1);
    psynder::net::voice::mute(2);

    std::array<std::uint8_t, 4000> mixed_out{};
    const auto mixed_sz = psynder::net::voice::mix_for_listener(
        speakers, 2, listener_pos, mixed_out.data(), 4000);

    // Both muted → no audio → 0 bytes returned.
    CHECK(mixed_sz == 0);

    // Unmute for subsequent tests.
    psynder::net::voice::unmute(1);
    psynder::net::voice::unmute(2);

    psynder::net::voice::shutdown_client();
}

// ─────────────────────────────────────────────────────────────────────────
// TEST 2: Both speakers unmuted, equidistant → output is non-silent
// ─────────────────────────────────────────────────────────────────────────
TEST_CASE("net-voice: equidistant unmuted speakers produce non-silent mix",
          "[net][voice][mix]") {

    psynder::net::voice::init_client({});

    int err = 0;
    OpusEncoder* enc1 = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    REQUIRE(err == OPUS_OK);
    OpusEncoder* enc2 = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    REQUIRE(err == OPUS_OK);

    float pcm1[kFrameSamples], pcm2[kFrameSamples];
    make_sine_float(pcm1, 440.0f, 0.5f);
    make_sine_float(pcm2, 880.0f, 0.5f);

    std::array<std::uint8_t, 4000> pkt1_buf, pkt2_buf;
    const auto sz1 = encode_float(enc1, pcm1, pkt1_buf.data(), 4000);
    const auto sz2 = encode_float(enc2, pcm2, pkt2_buf.data(), 4000);
    REQUIRE(sz1 > 0);
    REQUIRE(sz2 > 0);

    opus_encoder_destroy(enc1);
    opus_encoder_destroy(enc2);

    // Listener at origin; speakers equidistant at 1 m.
    const float listener_pos[3] = {0.0f, 0.0f, 0.0f};

    psynder::net::voice::MixSpeaker speakers[2] = {};
    speakers[0].speaker_id   = 10;
    speakers[0].opus_data    = pkt1_buf.data();
    speakers[0].opus_size    = sz1;
    speakers[0].world_pos[0] = 1.0f; speakers[0].world_pos[1] = 0.0f; speakers[0].world_pos[2] = 0.0f;

    speakers[1].speaker_id   = 11;
    speakers[1].opus_data    = pkt2_buf.data();
    speakers[1].opus_size    = sz2;
    speakers[1].world_pos[0] = -1.0f; speakers[1].world_pos[1] = 0.0f; speakers[1].world_pos[2] = 0.0f;

    // Both unmuted.
    psynder::net::voice::unmute(10);
    psynder::net::voice::unmute(11);

    std::array<std::uint8_t, 4000> mixed_out{};
    const auto mixed_sz = psynder::net::voice::mix_for_listener(
        speakers, 2, listener_pos, mixed_out.data(), 4000);

    REQUIRE(mixed_sz > 0);  // got a packet

    // Decode the result and verify it has significant energy (non-silent).
    OpusDecoder* dec = opus_decoder_create(kRate, 1, &err);
    REQUIRE(err == OPUS_OK);

    float decoded[kFrameSamples];
    const int n = decode_float(dec, mixed_out.data(), mixed_sz, decoded);
    REQUIRE(n == kFrameSamples);

    // Must have non-trivial energy: RMS well above noise floor.
    const double e = rms(decoded, kFrameSamples);
    CHECK(e > 1e-3);   // clearly above silence

    opus_decoder_destroy(dec);
    psynder::net::voice::shutdown_client();
}

// ─────────────────────────────────────────────────────────────────────────
// TEST 3: VAD gate — energy above threshold → packet; below → 0
// ─────────────────────────────────────────────────────────────────────────
TEST_CASE("net-voice: VAD gate opens on loud signal, suppresses silence",
          "[net][voice][vad]") {

    using namespace psynder::net::voice;

    psynder::net::voice::VoiceDesc desc{};
    desc.capture = psynder::net::voice::CaptureMode::Vad;
    init_client(desc);

    // ── Silence frame: should be gated out ──────────────────────────────
    std::array<std::int16_t, 960> silent_frame{};
    std::memset(silent_frame.data(), 0, silent_frame.size() * sizeof(std::int16_t));

    std::array<std::uint8_t, 4000> out_buf{};
    const auto sz_silence = capture_frame(silent_frame.data(), 960,
                                          out_buf.data(), 4000);
    CHECK(sz_silence == 0);  // VAD gate closed; silence suppressed

    // ── Loud tone frame: gate should open ───────────────────────────────
    std::array<std::int16_t, 960> loud_frame{};
    // Amplitude ~0.5 of full scale → RMS ≈ 0.354 >> kVadOpen (0.003)
    make_sine_i16(loud_frame.data(), 440.0f, 0.5f);

    const auto sz_loud = capture_frame(loud_frame.data(), 960,
                                       out_buf.data(), 4000);
    CHECK(sz_loud > 0);  // VAD gate opened; packet produced

    // ── Another silence frame after open gate → closes gate ─────────────
    // (gate uses hysteresis so we need to reset it first — shutdown+reinit)
    shutdown_client();
    init_client(desc);

    // First loud → open gate
    const auto sz_loud2 = capture_frame(loud_frame.data(), 960,
                                        out_buf.data(), 4000);
    CHECK(sz_loud2 > 0);

    // Now silence → close gate (below kVadClose)
    const auto sz_sil2 = capture_frame(silent_frame.data(), 960,
                                       out_buf.data(), 4000);
    CHECK(sz_sil2 == 0);

    shutdown_client();
}
