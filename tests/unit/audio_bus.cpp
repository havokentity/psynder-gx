// SPDX-License-Identifier: MIT
//
// Psynder — lane 23 unit tests: submix-graph bus mixer.
//
// These tests exercise:
//   1. Default routing: a voice marked live in the BusRouter starts on
//      BusId::Sfx (matching Engine::play()'s contract).
//   2. Per-bus gain attenuation: setting `gain_linear = 0.5` halves the
//      bus output once the ramp completes.
//   3. Zipper-free gain ramp: a 1.0 → 0.0 instantaneous gain change
//      produces an output ramp whose per-sample derivative is bounded.
//   4. 1-pole LP filter: a 10 kHz sine through a 1 kHz LP is attenuated
//      by >= 12 dB.
//   5. Reverb send: setting `reverb_send = 0.5` routes half the bus
//      signal (mono-summed) into the global reverb input buffer.
//   6. Stale-voice routing: `route_voice_to_bus(stale_id, ...)` returns
//      false without crashing.
//   7. kMaxBuses boundary: setting gains on BusId 15 succeeds; BusId 16
//      (cast out-of-range) is a no-op.
//
// IMPORTANT — link-layer note:
//   We deliberately do NOT call Engine::start() / Engine::play() in this
//   TU. The shared `tests/unit/CMakeLists.txt` (owned by lane 25) does
//   not link `psynder_platform_macos`, but Audio.cpp's Engine::start()
//   transitively references that lane's CoreAudio glue. The lane-23
//   bus surface lives in a separate TU (BusApi.cpp) precisely so tests
//   can exercise routing semantics by driving the BusRouter directly via
//   `mark_voice_live` / `mark_voice_dead` — which mirror what Engine::
//   play() / stop_voice() do — without dragging Audio.cpp.o into the
//   link.

#include "audio/Audio.h"             // VoiceId, ClipId
#include "audio/Bus.h"
#include "audio/BusMixer.h"
#include "audio/internal/BusRouter.h"
#include "audio/internal/BusTestHook.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using psynder::f32;
using psynder::u32;
using psynder::audio::BusGains;
using psynder::audio::BusId;
using psynder::audio::kMaxBuses;
using psynder::audio::VoiceId;
using psynder::audio::detail::BusMixer;
using psynder::audio::detail::kRampSamples;
using psynder::audio::detail::kLpBypassHz;

namespace {

// VoicePool pack/unpack matching MixerCore.h. Duplicated here so the
// test TU doesn't pull in the heavy MixerCore.h header.
inline u32 pack_voice_id(u32 idx, u32 gen) noexcept {
    return (idx & 0x00FFFFFFu) | ((gen & 0xFFu) << 24);
}

// Render a steady DC signal into a stereo buffer (so we can verify gain
// without any sample-rate-dependent math). DC = constant amplitude, so
// the LP filter at any cutoff converges to the same value.
std::vector<f32> make_dc_stereo(u32 frames, f32 amp) {
    std::vector<f32> buf(2u * frames, 0.0f);
    for (u32 i = 0; i < frames; ++i) {
        buf[2*i + 0] = amp;
        buf[2*i + 1] = amp;
    }
    return buf;
}

// Render a stereo sine of given frequency.
std::vector<f32> make_sine_stereo(u32 frames, f32 freq_hz, u32 sr, f32 amp) {
    std::vector<f32> buf(2u * frames, 0.0f);
    const f32 omega = 2.0f * 3.14159265358979323846f * freq_hz / static_cast<f32>(sr);
    for (u32 i = 0; i < frames; ++i) {
        const f32 s = std::sin(omega * static_cast<f32>(i)) * amp;
        buf[2*i + 0] = s;
        buf[2*i + 1] = s;
    }
    return buf;
}

}  // namespace

// ───────────────────────────────────────────────────────────────────────
// 1. Default routing — BusRouter shadow
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("audio: mark_voice_live defaults bus assignment to Sfx", "[audio][bus]") {
    using namespace psynder::audio;
    detail::router_reset();

    // Mark slot 3 live with gen 1 (matches what Engine::play() would do).
    detail::mark_voice_live(/*slot*/ 3u, /*gen*/ 1u);

    const VoiceId v{ pack_voice_id(3u, 1u) };
    REQUIRE(voice_bus(v) == BusId::Sfx);

    detail::mark_voice_dead(3u);
    detail::router_reset();
}

TEST_CASE("audio: route_voice_to_bus updates routing for live voice", "[audio][bus]") {
    using namespace psynder::audio;
    detail::router_reset();

    detail::mark_voice_live(5u, 7u);
    const VoiceId v{ pack_voice_id(5u, 7u) };

    REQUIRE(voice_bus(v) == BusId::Sfx);

    REQUIRE(route_voice_to_bus(v, BusId::Music));
    REQUIRE(voice_bus(v) == BusId::Music);

    REQUIRE(route_voice_to_bus(v, BusId::Ambient));
    REQUIRE(voice_bus(v) == BusId::Ambient);

    detail::mark_voice_dead(5u);
    detail::router_reset();
}

// ───────────────────────────────────────────────────────────────────────
// 2. Gain attenuation (DC signal through standalone BusMixer)
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("audio: bus gain_linear 0.5 halves output on DC signal", "[audio][bus]") {
    BusMixer mixer;
    REQUIRE(mixer.init(48000u, 256u));

    // First, settle the bus at unity gain so the ramp doesn't bias the
    // measurement.
    BusGains g{};
    g.gain_linear = 1.0f;
    g.lp_hz       = kLpBypassHz;
    g.reverb_send = 0.0f;
    mixer.set_target(BusId::Sfx, g);

    // Run several pulls at unity so the current gain reaches 1.0.
    constexpr u32 kSettleFrames = 256;
    const auto dc = make_dc_stereo(kSettleFrames, 0.25f);
    std::vector<f32> master(2u * kSettleFrames, 0.0f);
    std::vector<f32> rsend (kSettleFrames, 0.0f);
    for (u32 p = 0; p < 4; ++p) {
        std::fill(master.begin(), master.end(), 0.0f);
        std::fill(rsend.begin(),  rsend.end(),  0.0f);
        mixer.begin_frame(kSettleFrames);
        mixer.accumulate_voice(BusId::Sfx, dc.data(), kSettleFrames);
        mixer.mix_buses(master.data(), rsend.data(), kSettleFrames);
    }
    // At unity, master ≈ DC input (0.25 each channel).
    REQUIRE(std::fabs(master[0]                  - 0.25f) < 1e-4f);
    REQUIRE(std::fabs(master[2u * (kSettleFrames - 1u)] - 0.25f) < 1e-4f);

    // Now drop to 0.5 and run more pulls so the ramp completes.
    g.gain_linear = 0.5f;
    mixer.set_target(BusId::Sfx, g);
    for (u32 p = 0; p < 8; ++p) {
        std::fill(master.begin(), master.end(), 0.0f);
        std::fill(rsend.begin(),  rsend.end(),  0.0f);
        mixer.begin_frame(kSettleFrames);
        mixer.accumulate_voice(BusId::Sfx, dc.data(), kSettleFrames);
        mixer.mix_buses(master.data(), rsend.data(), kSettleFrames);
    }
    // After settle, master ≈ 0.5 * 0.25 = 0.125.
    const f32 settled_l = master[2u * (kSettleFrames - 1u)];
    const f32 settled_r = master[2u * (kSettleFrames - 1u) + 1u];
    INFO("settled L=" << settled_l << " R=" << settled_r);
    REQUIRE(std::fabs(settled_l - 0.125f) < 1e-4f);
    REQUIRE(std::fabs(settled_r - 0.125f) < 1e-4f);

    // And the pre-gain accumulator is the raw DC input (verifies routing).
    const f32* acc = mixer.bus_accumulator(BusId::Sfx);
    REQUIRE(acc != nullptr);
    REQUIRE(std::fabs(acc[0] - 0.25f) < 1e-6f);
}

// ───────────────────────────────────────────────────────────────────────
// 3. Zipper-free gain ramp
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("audio: gain ramp 1.0 → 0.0 has bounded first-derivative", "[audio][bus]") {
    BusMixer mixer;
    constexpr u32 kFrames = 512;  // >= kRampSamples (256) so ramp completes
    REQUIRE(mixer.init(48000u, kFrames));

    // Settle at unity first.
    BusGains g{};
    g.gain_linear = 1.0f;
    g.lp_hz       = kLpBypassHz;
    mixer.set_target(BusId::Sfx, g);
    const auto dc = make_dc_stereo(kFrames, 0.5f);
    std::vector<f32> master(2u * kFrames, 0.0f);
    std::vector<f32> rsend (kFrames, 0.0f);
    for (u32 p = 0; p < 4; ++p) {
        std::fill(master.begin(), master.end(), 0.0f);
        std::fill(rsend.begin(),  rsend.end(),  0.0f);
        mixer.begin_frame(kFrames);
        mixer.accumulate_voice(BusId::Sfx, dc.data(), kFrames);
        mixer.mix_buses(master.data(), rsend.data(), kFrames);
    }
    // Confirm settled at unity.
    REQUIRE(std::fabs(master[0] - 0.5f) < 1e-4f);

    // Now hop target to 0 and capture one pull.
    g.gain_linear = 0.0f;
    mixer.set_target(BusId::Sfx, g);

    std::fill(master.begin(), master.end(), 0.0f);
    std::fill(rsend.begin(),  rsend.end(),  0.0f);
    mixer.begin_frame(kFrames);
    mixer.accumulate_voice(BusId::Sfx, dc.data(), kFrames);
    mixer.mix_buses(master.data(), rsend.data(), kFrames);

    // The ramp should complete within kRampSamples (256). By the time we
    // hit frame `kRampSamples` the gain should be 0 and the master
    // sample should be ~0.
    REQUIRE(std::fabs(master[2u * kRampSamples] - 0.0f) < 1e-4f);
    REQUIRE(std::fabs(master[2u * (kFrames - 1u)] - 0.0f) < 1e-4f);

    // L2 norm of the first-derivative (per-sample diff) should be small —
    // bounded by amp / kRampSamples ≈ 0.5/256 ≈ 0.00195 per sample, times
    // sqrt(kRampSamples). Pure linear ramp from 0.5 to 0 over 256 samples:
    // d/dn = -0.5 / 256 = -0.001953. L2 over the ramp = 0.001953 * sqrt(256)
    // = 0.03125. We assert the L2 ≤ 0.1 with comfortable headroom.
    f32 l2_sq = 0.0f;
    for (u32 i = 1; i < kFrames; ++i) {
        const f32 d = master[2*i] - master[2*(i-1)];
        l2_sq += d * d;
    }
    const f32 l2 = std::sqrt(l2_sq);
    INFO("First-derivative L2 norm: " << l2);
    REQUIRE(l2 < 0.1f);

    // And the worst per-sample diff (linf) should be tiny — definitely
    // smaller than 0.01 (a click would be ~0.5 in one sample).
    f32 max_diff = 0.0f;
    for (u32 i = 1; i < kFrames; ++i) {
        max_diff = std::fmax(max_diff, std::fabs(master[2*i] - master[2*(i-1)]));
    }
    INFO("Max per-sample diff: " << max_diff);
    REQUIRE(max_diff < 0.01f);
}

// ───────────────────────────────────────────────────────────────────────
// 4. 1-pole LP filter
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("audio: bus LP @ 1 kHz attenuates 10 kHz by >= 12 dB", "[audio][bus]") {
    BusMixer mixer;
    constexpr u32 kSr     = 48000;
    constexpr u32 kFrames = 1024;
    REQUIRE(mixer.init(kSr, kFrames));

    // Reference pass: bypass LP, record output amplitude.
    BusGains g{};
    g.gain_linear = 1.0f;
    g.lp_hz       = kLpBypassHz;  // bypass
    g.reverb_send = 0.0f;
    mixer.set_target(BusId::Music, g);

    // Settle.
    const auto sine = make_sine_stereo(kFrames, 10000.0f, kSr, 0.5f);
    std::vector<f32> master(2u * kFrames, 0.0f);
    std::vector<f32> rsend (kFrames, 0.0f);
    for (u32 p = 0; p < 4; ++p) {
        std::fill(master.begin(), master.end(), 0.0f);
        std::fill(rsend.begin(),  rsend.end(),  0.0f);
        mixer.begin_frame(kFrames);
        mixer.accumulate_voice(BusId::Music, sine.data(), kFrames);
        mixer.mix_buses(master.data(), rsend.data(), kFrames);
    }
    f32 ref_peak = 0.0f;
    // Skip the first 64 samples (settling) when measuring.
    for (u32 i = 64; i < kFrames; ++i) {
        ref_peak = std::fmax(ref_peak, std::fabs(master[2*i]));
    }
    INFO("Bypass peak: " << ref_peak);
    REQUIRE(ref_peak > 0.4f);   // ~0.5 amp, give some headroom

    // Now apply 1 kHz LP and measure.
    g.lp_hz = 1000.0f;
    mixer.set_target(BusId::Music, g);
    for (u32 p = 0; p < 4; ++p) {
        std::fill(master.begin(), master.end(), 0.0f);
        std::fill(rsend.begin(),  rsend.end(),  0.0f);
        mixer.begin_frame(kFrames);
        mixer.accumulate_voice(BusId::Music, sine.data(), kFrames);
        mixer.mix_buses(master.data(), rsend.data(), kFrames);
    }
    f32 lp_peak = 0.0f;
    for (u32 i = 64; i < kFrames; ++i) {
        lp_peak = std::fmax(lp_peak, std::fabs(master[2*i]));
    }
    INFO("LP peak: " << lp_peak);
    REQUIRE(lp_peak > 0.0f);

    // dB attenuation = 20 * log10(ref / lp). Demand >= 12 dB.
    const f32 atten_db = 20.0f * std::log10(ref_peak / lp_peak);
    INFO("Attenuation: " << atten_db << " dB");
    REQUIRE(atten_db >= 12.0f);
}

// ───────────────────────────────────────────────────────────────────────
// 5. Reverb send
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("audio: bus reverb_send 0.5 routes half signal into reverb input", "[audio][bus]") {
    BusMixer mixer;
    constexpr u32 kFrames = 256;
    REQUIRE(mixer.init(48000u, kFrames));

    // Configure SFX bus: unity gain, no LP, reverb_send = 0.5.
    BusGains g{};
    g.gain_linear = 1.0f;
    g.lp_hz       = kLpBypassHz;
    g.reverb_send = 0.5f;
    mixer.set_target(BusId::Sfx, g);

    // Settle.
    const auto dc = make_dc_stereo(kFrames, 0.4f);
    std::vector<f32> master(2u * kFrames, 0.0f);
    std::vector<f32> rsend (kFrames, 0.0f);
    for (u32 p = 0; p < 4; ++p) {
        std::fill(master.begin(), master.end(), 0.0f);
        std::fill(rsend.begin(),  rsend.end(),  0.0f);
        mixer.begin_frame(kFrames);
        mixer.accumulate_voice(BusId::Sfx, dc.data(), kFrames);
        mixer.mix_buses(master.data(), rsend.data(), kFrames);
    }

    // The reverb-input buffer holds 0.5 * mono(bus_out). mono(DC stereo)
    // is 0.5 * (L + R) = 0.5 * (0.4 + 0.4) = 0.4. Send * mono = 0.5 * 0.4
    // = 0.2.
    INFO("Reverb send sample: " << rsend[kFrames / 2]);
    REQUIRE(std::fabs(rsend[kFrames / 2] - 0.2f) < 1e-3f);
    REQUIRE(std::fabs(rsend[kFrames - 1u] - 0.2f) < 1e-3f);

    // The BusMixer's internal reverb-send buffer mirrors the engine's:
    // verify it matches via the test hook.
    const f32* rsend_internal = mixer.reverb_send_buffer();
    REQUIRE(rsend_internal != nullptr);
    REQUIRE(std::fabs(rsend_internal[kFrames - 1u] - 0.2f) < 1e-3f);
}

TEST_CASE("audio: bus reverb_send 0.0 produces no reverb input", "[audio][bus]") {
    BusMixer mixer;
    constexpr u32 kFrames = 256;
    REQUIRE(mixer.init(48000u, kFrames));

    BusGains g{};
    g.gain_linear = 1.0f;
    g.lp_hz       = kLpBypassHz;
    g.reverb_send = 0.0f;
    mixer.set_target(BusId::Sfx, g);

    const auto dc = make_dc_stereo(kFrames, 0.5f);
    std::vector<f32> master(2u * kFrames, 0.0f);
    std::vector<f32> rsend (kFrames, 0.0f);
    mixer.begin_frame(kFrames);
    mixer.accumulate_voice(BusId::Sfx, dc.data(), kFrames);
    mixer.mix_buses(master.data(), rsend.data(), kFrames);

    for (u32 i = 0; i < kFrames; ++i) {
        REQUIRE(rsend[i] == 0.0f);
    }
}

// ───────────────────────────────────────────────────────────────────────
// 6. Stale voice routing
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("audio: route_voice_to_bus on stale id returns false", "[audio][bus]") {
    using namespace psynder::audio;
    detail::router_reset();

    // 1) Zero id always fails.
    REQUIRE_FALSE(route_voice_to_bus(VoiceId{0u}, BusId::Music));

    // 2) Slot was never marked live.
    const VoiceId never_played{ pack_voice_id(0u, 1u) };
    REQUIRE_FALSE(route_voice_to_bus(never_played, BusId::Music));

    // 3) Mark slot 0 live with gen 1, then mark dead — routing must fail.
    detail::mark_voice_live(0u, 1u);
    const VoiceId v1{ pack_voice_id(0u, 1u) };
    REQUIRE(route_voice_to_bus(v1, BusId::Music));   // live
    detail::mark_voice_dead(0u);
    REQUIRE_FALSE(route_voice_to_bus(v1, BusId::Music)); // dead

    // 4) Re-mark live with bumped gen — original id is stale.
    detail::mark_voice_live(0u, 2u);
    const VoiceId v2{ pack_voice_id(0u, 2u) };
    REQUIRE(route_voice_to_bus(v2, BusId::Voice));        // new id OK
    REQUIRE_FALSE(route_voice_to_bus(v1, BusId::Voice));  // old id stale

    // 5) Bogus id with garbage in the high bits / out-of-range slot.
    REQUIRE_FALSE(route_voice_to_bus(VoiceId{0xDEADBEEFu}, BusId::Music));

    detail::router_reset();
}

// ───────────────────────────────────────────────────────────────────────
// 7. kMaxBuses boundary
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("audio: setting gains on BusId 15 succeeds", "[audio][bus]") {
    BusMixer mixer;
    REQUIRE(mixer.init(48000u, 256u));

    BusGains g{};
    g.gain_linear = 0.75f;
    g.lp_hz       = 8000.0f;
    g.reverb_send = 0.25f;
    const BusId max_bus = static_cast<BusId>(kMaxBuses - 1u);
    mixer.set_target(max_bus, g);

    const BusGains got = mixer.get_target(max_bus);
    REQUIRE(std::fabs(got.gain_linear - 0.75f) < 1e-6f);
    REQUIRE(std::fabs(got.lp_hz       - 8000.0f) < 1e-3f);
    REQUIRE(std::fabs(got.reverb_send - 0.25f) < 1e-6f);
}

TEST_CASE("audio: setting gains on out-of-range BusId is a no-op", "[audio][bus]") {
    BusMixer mixer;
    REQUIRE(mixer.init(48000u, 256u));

    BusGains g{};
    g.gain_linear = 0.42f;
    g.lp_hz       = 1234.0f;
    g.reverb_send = 0.42f;
    // Cast a value out of range (kMaxBuses = 16, so 16 is invalid).
    const BusId oob = static_cast<BusId>(kMaxBuses);
    mixer.set_target(oob, g);

    // The getter for an out-of-range bus returns the default-constructed
    // BusGains, NOT what we tried to write.
    const BusGains got = mixer.get_target(oob);
    REQUIRE(got.gain_linear == 1.0f);
    REQUIRE(got.lp_hz       == 22000.0f);
    REQUIRE(got.reverb_send == 0.0f);

    // And the in-range buses are untouched.
    const BusGains sfx = mixer.get_target(BusId::Sfx);
    REQUIRE(sfx.gain_linear == 1.0f);
}

TEST_CASE("audio: accumulate_voice on out-of-range bus is a no-op", "[audio][bus]") {
    BusMixer mixer;
    constexpr u32 kFrames = 64;
    REQUIRE(mixer.init(48000u, kFrames));

    mixer.begin_frame(kFrames);
    const auto dc = make_dc_stereo(kFrames, 1.0f);
    // Should NOT crash, NOT write anywhere.
    mixer.accumulate_voice(static_cast<BusId>(kMaxBuses + 7u), dc.data(), kFrames);

    // All accumulators stay zero.
    for (u32 b = 0; b < kMaxBuses; ++b) {
        const f32* acc = mixer.bus_accumulator(static_cast<BusId>(b));
        REQUIRE(acc != nullptr);
        for (u32 k = 0; k < 2u * kFrames; ++k) {
            REQUIRE(acc[k] == 0.0f);
        }
    }
}

// ───────────────────────────────────────────────────────────────────────
// Bonus: clamp behaviour for BusGains
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("audio: BusGains clamping rejects negative / over-range inputs", "[audio][bus]") {
    BusMixer mixer;
    REQUIRE(mixer.init(48000u, 256u));

    BusGains g{};
    g.gain_linear = -1.0f;   // clamped to 0
    g.lp_hz       = 50000.0f; // clamped to kLpBypassHz
    g.reverb_send = 5.0f;     // clamped to 1
    mixer.set_target(BusId::Voice, g);

    const BusGains got = mixer.get_target(BusId::Voice);
    REQUIRE(got.gain_linear == 0.0f);
    REQUIRE(got.lp_hz       == kLpBypassHz);
    REQUIRE(got.reverb_send == 1.0f);

    BusGains g2{};
    g2.gain_linear = 10.0f;   // clamped to 4
    g2.lp_hz       = 0.0f;    // clamped to 10 (LP cutoff floor)
    g2.reverb_send = -0.5f;   // clamped to 0
    mixer.set_target(BusId::Voice, g2);

    const BusGains got2 = mixer.get_target(BusId::Voice);
    REQUIRE(got2.gain_linear == 4.0f);
    REQUIRE(got2.lp_hz       == 10.0f);
    REQUIRE(got2.reverb_send == 0.0f);
}

// ───────────────────────────────────────────────────────────────────────
// Bonus: engine_bus_mixer test hook + set_bus_gains plumbing
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("audio: engine_bus_mixer test hook returns the singleton", "[audio][bus]") {
    using namespace psynder::audio;
    detail::router_reset();

    detail::BusMixer* m = detail::engine_bus_mixer();
    REQUIRE(m != nullptr);

    BusGains g{};
    g.gain_linear = 0.33f;
    g.lp_hz       = 4000.0f;
    set_bus_gains(BusId::Ambient, g);

    const BusGains got = m->get_target(BusId::Ambient);
    REQUIRE(std::fabs(got.gain_linear - 0.33f) < 1e-6f);
    REQUIRE(std::fabs(got.lp_hz       - 4000.0f) < 1e-3f);

    // And the free-function getter agrees.
    const BusGains via_api = get_bus_gains(BusId::Ambient);
    REQUIRE(std::fabs(via_api.gain_linear - 0.33f) < 1e-6f);

    detail::router_reset();
}

TEST_CASE("audio: voice_bus returns Sfx for invalid id", "[audio][bus]") {
    using namespace psynder::audio;
    detail::router_reset();

    REQUIRE(voice_bus(VoiceId{0u}) == BusId::Sfx);
    REQUIRE(voice_bus(VoiceId{0xDEADBEEFu}) == BusId::Sfx);
    // Slot 1, gen 5, never marked live — returns the Sfx default.
    REQUIRE(voice_bus(VoiceId{ pack_voice_id(1u, 5u) }) == BusId::Sfx);

    detail::router_reset();
}
