// SPDX-License-Identifier: MIT
//
// engine/audio/BusApi.cpp
//
// Lane 23 — public bus API + global router singleton.
//
// Why is this not in Audio.cpp?
//   The voice-pool / device-backend lives in Audio.cpp.o, which transitively
//   references the platform lane's CoreAudio glue (lane 25's
//   default_audio_device_name / default_audio_sample_rate strong symbols).
//   The shared unit-test binary links psynder_audio but NOT
//   psynder_platform_macos, so any test TU that references a symbol from
//   Audio.cpp.o would drag in unresolved CoreAudio externs at link time.
//
//   By housing the Bus.h free functions and the router singleton in a
//   separate TU, tests can drive route_voice_to_bus / set_bus_gains /
//   engine_bus_mixer without forcing Audio.cpp.o into the link, keeping
//   the lane-25-owned tests/unit/CMakeLists.txt untouched.

#include "audio/Bus.h"
#include "audio/BusMixer.h"
#include "audio/internal/BusRouter.h"
#include "audio/internal/BusTestHook.h"

#include "core/Types.h"

#include <array>
#include <cstdint>
#include <mutex>

namespace psynder::audio {

namespace detail {

namespace {

// Per-voice-slot liveness shadow + bus assignment. Independent of the
// voice pool in Audio.cpp — Engine::play()/stop_voice() drive these via
// mark_voice_live / mark_voice_dead.
struct VoiceSlotShadow {
    bool        active = false;
    std::uint8_t gen   = 0;  // matches the 8-bit gen in VoicePool's packed id
    BusId       bus    = BusId::Sfx;
};

struct RouterState {
    BusMixer                                            mixer{};
    std::array<VoiceSlotShadow, kBusRouterMaxVoices>    slots{};
    mutable std::mutex                                  slots_mu{};
};

RouterState& router_state() noexcept {
    static RouterState s;
    return s;
}

// Voice-id pack/unpack — duplicated from MixerCore.h's VoicePool to avoid
// pulling in that header (which transitively brings in HRTF / FFT defs).
inline std::uint32_t unpack_voice_index(std::uint32_t packed) noexcept {
    return packed & 0x00FFFFFFu;
}
inline std::uint32_t unpack_voice_gen(std::uint32_t packed) noexcept {
    return (packed >> 24) & 0xFFu;
}

}  // namespace

// ─── Router singleton API ─────────────────────────────────────────────────

BusMixer& bus_mixer() noexcept {
    return router_state().mixer;
}

void router_reset() noexcept {
    RouterState& r = router_state();
    {
        std::lock_guard<std::mutex> lk(r.slots_mu);
        for (auto& s : r.slots) {
            s.active = false;
            s.gen    = 0;
            s.bus    = BusId::Sfx;
        }
    }
    r.mixer.reset();
}

bool router_init_mixer(u32 sample_rate, u32 max_frames) noexcept {
    return router_state().mixer.init(sample_rate, max_frames);
}

void mark_voice_live(u32 slot_idx, u32 generation) noexcept {
    if (slot_idx >= kBusRouterMaxVoices) return;
    RouterState& r = router_state();
    std::lock_guard<std::mutex> lk(r.slots_mu);
    r.slots[slot_idx].active = true;
    r.slots[slot_idx].gen    = static_cast<std::uint8_t>(generation & 0xFFu);
    r.slots[slot_idx].bus    = BusId::Sfx;  // reset to default on each play()
}

void mark_voice_dead(u32 slot_idx) noexcept {
    if (slot_idx >= kBusRouterMaxVoices) return;
    RouterState& r = router_state();
    std::lock_guard<std::mutex> lk(r.slots_mu);
    r.slots[slot_idx].active = false;
}

BusId slot_bus(u32 slot_idx) noexcept {
    if (slot_idx >= kBusRouterMaxVoices) return BusId::Sfx;
    RouterState& r = router_state();
    std::lock_guard<std::mutex> lk(r.slots_mu);
    return r.slots[slot_idx].bus;
}

// ─── Test hook ───────────────────────────────────────────────────────────

BusMixer* engine_bus_mixer() noexcept {
    return &router_state().mixer;
}

}  // namespace detail

// ─── Public bus API (Bus.h) ───────────────────────────────────────────────

void set_bus_gains(BusId bus, const BusGains& g) noexcept {
    detail::router_state().mixer.set_target(bus, g);
}

BusGains get_bus_gains(BusId bus) noexcept {
    return detail::router_state().mixer.get_target(bus);
}

bool route_voice_to_bus(VoiceId voice, BusId bus) noexcept {
    if (voice.raw == 0u) return false;
    if (static_cast<u32>(bus) >= kMaxBuses) return false;
    const u32 idx = detail::unpack_voice_index(voice.raw);
    const u32 gen = detail::unpack_voice_gen(voice.raw);
    if (idx >= detail::kBusRouterMaxVoices) return false;

    detail::RouterState& r = detail::router_state();
    std::lock_guard<std::mutex> lk(r.slots_mu);
    const detail::VoiceSlotShadow& s = r.slots[idx];
    if (!s.active || s.gen != static_cast<std::uint8_t>(gen & 0xFFu)) {
        return false;  // stale or never-played
    }
    r.slots[idx].bus = bus;
    return true;
}

BusId voice_bus(VoiceId voice) noexcept {
    if (voice.raw == 0u) return BusId::Sfx;
    const u32 idx = detail::unpack_voice_index(voice.raw);
    const u32 gen = detail::unpack_voice_gen(voice.raw);
    if (idx >= detail::kBusRouterMaxVoices) return BusId::Sfx;

    detail::RouterState& r = detail::router_state();
    std::lock_guard<std::mutex> lk(r.slots_mu);
    const detail::VoiceSlotShadow& s = r.slots[idx];
    if (!s.active || s.gen != static_cast<std::uint8_t>(gen & 0xFFu)) {
        return BusId::Sfx;
    }
    return s.bus;
}

}  // namespace psynder::audio
