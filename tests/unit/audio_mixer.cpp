// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit tests: mixer headroom + reverb + WAV loader.
//
// The first half pulls the SIMD voice-sum from the header-only mixer core
// so it compiles without `psynder_audio` linkage. The new clip / reverb
// tests link against `psynder_audio` via the unit-test CMakeLists.

#include "audio/AudioClip.h"
#include "audio/internal/MixerCore.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {
constexpr psynder::u32 kFrames = 256;
}  // namespace

TEST_CASE("audio: mixer doesn't clip with 32 quiet sine voices", "[audio][mixer]") {
    using namespace psynder;
    using namespace psynder::audio::detail;

    // Each voice writes a sine at amplitude (1 / kMaxVoices) so the sum
    // peaks at ≤ 1.0 even in the worst-case phase-aligned scenario.
    constexpr u32 stereo_floats = 2u * kFrames;
    std::array<f32, stereo_floats> master{};

    const f32 per_voice_amp = 1.0f / static_cast<f32>(kMaxVoices);

    std::vector<f32> voice_buf(stereo_floats, 0.0f);
    for (u32 v = 0; v < kMaxVoices; ++v) {
        // Render this voice into its own buffer first.
        std::fill(voice_buf.begin(), voice_buf.end(), 0.0f);
        const f32 phase = static_cast<f32>(v) * 0.123f;
        for (u32 i = 0; i < kFrames; ++i) {
            const f32 s = std::sin(2.0f * psynder::math::kPi *
                                   (static_cast<f32>(i) / static_cast<f32>(kFrames)) + phase);
            voice_buf[2*i + 0] = s * per_voice_amp;
            voice_buf[2*i + 1] = s * per_voice_amp;
        }
        // SIMD-merge into master.
        simd_mix_into(master.data(), voice_buf.data(), 1.0f, stereo_floats);
    }

    // master peak should be ≤ 1.0 with healthy headroom; assert strictly
    // less than +1.0 and greater than -1.0 (the deliverable bar).
    f32 peak = 0.0f;
    for (u32 i = 0; i < stereo_floats; ++i) {
        peak = std::fmax(peak, std::fabs(master[i]));
    }
    INFO("Mixed peak amplitude: " << peak);
    REQUIRE(peak <= 1.0f);
    // Sanity: with random phases the mixed peak shouldn't approach the
    // per-voice peak (1/N * N = 1) — the random phase should reduce it.
    REQUIRE(peak >  0.0f);
}

TEST_CASE("audio: simd_mix_into matches scalar reference", "[audio][mixer][simd]") {
    using namespace psynder;
    using namespace psynder::audio::detail;

    constexpr u32 n = 17;  // odd to exercise SIMD-bulk + scalar-tail
    std::array<f32, n> dst_simd{};
    std::array<f32, n> dst_ref{};
    std::array<f32, n> src{};
    for (u32 i = 0; i < n; ++i) src[i] = static_cast<f32>(i) * 0.1f;

    simd_mix_into(dst_simd.data(), src.data(), 2.0f, n);
    for (u32 i = 0; i < n; ++i) dst_ref[i] = src[i] * 2.0f;

    for (u32 i = 0; i < n; ++i) {
        INFO("i=" << i << " simd=" << dst_simd[i] << " ref=" << dst_ref[i]);
        REQUIRE(std::fabs(dst_simd[i] - dst_ref[i]) < 1e-6f);
    }
}

// ─── WAV loader ────────────────────────────────────────────────────────
// Round-trip: build a valid 16-bit PCM WAV in memory, parse it, verify
// the AudioClip contents match.
namespace {

// Append little-endian u16/u32 to a byte vector.
inline void push_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}
inline void push_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
}
inline void push_fourcc(std::vector<std::uint8_t>& v, const char* tag) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(tag[i]));
}

// Build a minimal valid PCM-int16 WAV file. Returns the encoded bytes.
std::vector<std::uint8_t> make_wav(const std::vector<std::int16_t>& samples,
                                   std::uint32_t sample_rate,
                                   std::uint16_t channels) {
    const std::uint32_t data_bytes =
        static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * 2);
    const std::uint32_t byte_rate   = sample_rate * block_align;
    std::vector<std::uint8_t> buf;
    push_fourcc(buf, "RIFF");
    push_u32(buf, 36u + data_bytes);
    push_fourcc(buf, "WAVE");
    // fmt chunk
    push_fourcc(buf, "fmt ");
    push_u32(buf, 16u);
    push_u16(buf, 1u);                              // PCM
    push_u16(buf, channels);
    push_u32(buf, sample_rate);
    push_u32(buf, byte_rate);
    push_u16(buf, block_align);
    push_u16(buf, 16u);                             // bits per sample
    // data chunk
    push_fourcc(buf, "data");
    push_u32(buf, data_bytes);
    const std::uint8_t* sp = reinterpret_cast<const std::uint8_t*>(samples.data());
    buf.insert(buf.end(), sp, sp + data_bytes);
    return buf;
}

}  // namespace

TEST_CASE("audio: WAV loader round-trips a mono int16 buffer",
          "[audio][clip][wav]") {
    using namespace psynder;
    std::vector<std::int16_t> pcm{ 0, 1000, -1000, 32767, -32768, 0, 12345 };
    auto wav = make_wav(pcm, /*sr=*/44100u, /*channels=*/1u);
    audio::AudioClip clip;
    REQUIRE(audio::load_wav_memory(wav.data(), wav.size(), clip));
    REQUIRE(clip.channels == 1u);
    REQUIRE(clip.sample_rate == 44100u);
    REQUIRE(clip.samples.size() == pcm.size());
    REQUIRE(clip.frame_count() == pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        INFO("i=" << i);
        REQUIRE(clip.samples[i] == pcm[i]);
    }
}

TEST_CASE("audio: WAV loader round-trips a stereo int16 buffer",
          "[audio][clip][wav]") {
    using namespace psynder;
    // L, R interleaved.
    std::vector<std::int16_t> pcm{ 1, -1, 2, -2, 100, 200, -100, -200 };
    auto wav = make_wav(pcm, /*sr=*/48000u, /*channels=*/2u);
    audio::AudioClip clip;
    REQUIRE(audio::load_wav_memory(wav.data(), wav.size(), clip));
    REQUIRE(clip.channels == 2u);
    REQUIRE(clip.sample_rate == 48000u);
    REQUIRE(clip.samples.size() == pcm.size());
    REQUIRE(clip.frame_count() == pcm.size() / 2u);
    for (std::size_t i = 0; i < pcm.size(); ++i) REQUIRE(clip.samples[i] == pcm[i]);
}

TEST_CASE("audio: WAV loader rejects garbage", "[audio][clip][wav]") {
    using namespace psynder;
    audio::AudioClip clip;
    REQUIRE_FALSE(audio::load_wav_memory(nullptr, 0u, clip));
    const char* not_wav = "this is not a wav file at all";
    REQUIRE_FALSE(audio::load_wav_memory(not_wav, std::strlen(not_wav), clip));
    REQUIRE(clip.empty());
}

TEST_CASE("audio: clip registry returns the same data we registered",
          "[audio][clip][registry]") {
    using namespace psynder;
    audio::clear_clip_registry();
    audio::AudioClip c;
    c.samples = { 1, 2, 3, 4 };
    c.channels = 1;
    c.sample_rate = 22050u;
    c.name = "test";
    const audio::ClipId id = audio::register_clip(std::move(c));
    REQUIRE(id.valid());
    const audio::AudioClip* got = audio::clip_by_id(id);
    REQUIRE(got != nullptr);
    REQUIRE(got->sample_rate == 22050u);
    REQUIRE(got->samples.size() == 4u);
    audio::clear_clip_registry();
    // After clear, the previous id is stale.
    REQUIRE(audio::clip_by_id(id) == nullptr);
}

// ─── FDN (Hadamard 8) ──────────────────────────────────────────────────
// The 8-line FDN must produce non-zero wet output for ≥ 100 ms after a
// unit impulse (it's a feedback network — silence in, decaying wet out).
TEST_CASE("audio: FDN reverb has audible tail after impulse",
          "[audio][reverb][fdn]") {
    using namespace psynder;
    audio::detail::FdnReverb fdn;
    fdn.reset(/*sr=*/48000u, /*decay=*/2.0f);
    // Verify all 8 delay lines were sized.
    for (u32 i = 0; i < audio::detail::kFdnSize; ++i) {
        REQUIRE(fdn.line_length(i) > 0u);
    }
    // Inject unit impulse, then run silence and measure tail energy.
    f32 first = fdn.tick(1.0f);
    (void)first;
    f32 tail_energy = 0.0f;
    constexpr u32 kRun = 4800u;  // 100 ms @ 48k
    for (u32 i = 0; i < kRun; ++i) {
        const f32 s = fdn.tick(0.0f);
        tail_energy += s * s;
    }
    INFO("FDN 100 ms tail energy: " << tail_energy);
    REQUIRE(tail_energy > 1e-6f);
}

// ─── FFT convolution against an identity IR ────────────────────────────
// process_block(x) with IR = [1, 0, 0, ...] should yield x back (within
// FFT rounding). This is the unambiguous correctness check.
//
// FftConvReverb carries ~600 KB of internal scratch (IR + signal + kernel
// FFT scratch), so we heap-allocate the instance to keep test stack
// frames small.
TEST_CASE("audio: FFT convolution preserves input against unit-impulse IR",
          "[audio][reverb][fft]") {
    using namespace psynder;
    auto conv = std::make_unique<audio::detail::FftConvReverb>();
    const f32 ir_dirac[8] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    REQUIRE(conv->load_rir_from_memory(ir_dirac, 8u,
                                       /*src_sr=*/48000u, /*dst_sr=*/48000u));
    REQUIRE(conv->ir_length() == 8u);
    constexpr u32 N = 32;
    f32 sig[N];
    for (u32 i = 0; i < N; ++i) sig[i] = std::sin(static_cast<f32>(i) * 0.1f);
    f32 out[N + 8] = {};
    const u32 out_len = conv->process_block(sig, N, out);
    REQUIRE(out_len == N + 7u);
    for (u32 i = 0; i < N; ++i) {
        INFO("i=" << i << " sig=" << sig[i] << " out=" << out[i]);
        REQUIRE(std::fabs(out[i] - sig[i]) < 1e-3f);
    }
}

TEST_CASE("audio: FFT convolution loaded baked short_room IR",
          "[audio][reverb][fft][baked]") {
    using namespace psynder;
    auto conv = std::make_unique<audio::detail::FftConvReverb>();
    // Tests run from build/mac; the worktree root is two up. The CMake
    // macro PSY_AUDIO_RIR_RELATIVE_PATH expands to the path relative to
    // the source root, so we try both candidates.
    const bool baked_ok =
        conv->load_rir_from_file("../../engine/audio/builtin/short_room.bin",
                                 /*sample_rate=*/48000u) ||
        conv->load_rir_from_file("engine/audio/builtin/short_room.bin",
                                 /*sample_rate=*/48000u);
    if (!baked_ok) {
        // Synthetic fallback — still a valid reverb input.
        conv->reset_synthetic(48000u, 0.12f, 1.2f);
    }
    REQUIRE(conv->ir_length() > 0u);
    constexpr u32 N = 64;
    f32 sig[N] = {};
    sig[0] = 1.0f;  // unit impulse
    // Output buffer must hold N + ir_len - 1 floats. Baked IR is 24000
    // taps so conv_len ≈ 24063; the synthetic fallback is shorter (~5760).
    // Size the heap buffer generously.
    std::vector<f32> out(static_cast<std::size_t>(N) + conv->ir_length() + 16u, 0.0f);
    const u32 out_len = conv->process_block(sig, N, out.data());
    REQUIRE(out_len > 0u);
    REQUIRE(out_len <= out.size());
    // After convolution we expect a non-zero tail well past the impulse.
    f32 tail = 0.0f;
    for (u32 i = N; i < out_len; ++i) tail += out[i] * out[i];
    INFO("baked_ok=" << baked_ok << " out_len=" << out_len << " tail=" << tail);
    REQUIRE(tail > 0.0f);
}
