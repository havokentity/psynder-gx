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
#include <atomic>
#include <cstdint>

namespace psynder::audio {

namespace detail {

namespace {

// Per-voice-slot state packed into a single std::atomic<uint32_t> so the
// audio callback's slot_bus() read is lock-free.  The pre-PR-#19 version
// used a per-pool std::mutex and the audio thread did a blocking
// lock_guard — which Copilot caught as an RT-deadline hazard: a UI
// thread mid-route_voice_to_bus would block the audio callback long
// enough to miss a Wasapi / CoreAudio / PipeWire frame.
//
// Bit layout (LSB → MSB):
//   bit  0   : active flag        (1 bit)
//   bits 1..8: generation         (8 bits, matches VoicePool's packed gen)
//   bits 9..16: bus id            (8 bits, supports up to 256 buses;
//                                  the public cap is kMaxBuses == 16)
//   bits 17..31: reserved (zero)
//
// Writers (mark_voice_live / _dead / route_voice_to_bus) use a CAS loop
// to preserve cross-field invariants — e.g. route_voice_to_bus refuses
// to update the bus on a slot whose active/gen has changed underneath it.
// Reader (slot_bus on the audio thread) is a single relaxed load + decode.
inline constexpr std::uint32_t kSlotActiveBit  = 1u << 0;
inline constexpr std::uint32_t kSlotGenShift   = 1u;
inline constexpr std::uint32_t kSlotGenMask    = 0xFFu << kSlotGenShift;
inline constexpr std::uint32_t kSlotBusShift   = 9u;
inline constexpr std::uint32_t kSlotBusMask    = 0xFFu << kSlotBusShift;

inline std::uint32_t pack_slot(bool active, std::uint8_t gen, BusId bus) noexcept {
    return (active ? kSlotActiveBit : 0u)
         | (static_cast<std::uint32_t>(gen) << kSlotGenShift)
         | (static_cast<std::uint32_t>(bus) << kSlotBusShift);
}
inline bool        unpack_active(std::uint32_t v) noexcept { return (v & kSlotActiveBit) != 0u; }
inline std::uint8_t unpack_gen(std::uint32_t v)   noexcept { return static_cast<std::uint8_t>((v & kSlotGenMask) >> kSlotGenShift); }
inline BusId       unpack_bus(std::uint32_t v)    noexcept { return static_cast<BusId>((v & kSlotBusMask) >> kSlotBusShift); }

struct VoiceSlotShadow {
    std::atomic<std::uint32_t> packed{0};
};

struct RouterState {
    BusMixer                                            mixer{};
    std::array<VoiceSlotShadow, kBusRouterMaxVoices>    slots{};
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
    // Atomic per-slot reset — no mutex.  Each store is release-ordered so
    // any subsequent acquire-load on the audio thread sees a coherent
    // zeroed slot.
    for (auto& s : r.slots) {
        s.packed.store(0u, std::memory_order_release);
    }
    r.mixer.reset();
}

bool router_init_mixer(u32 sample_rate, u32 max_frames) noexcept {
    return router_state().mixer.init(sample_rate, max_frames);
}

void mark_voice_live(u32 slot_idx, u32 generation) noexcept {
    if (slot_idx >= kBusRouterMaxVoices) return;
    RouterState& r = router_state();
    // Active=true, gen=given, bus=default Sfx.  Single store — no
    // intermediate "half-updated" state visible to the audio thread.
    const std::uint32_t new_val = pack_slot(
        /*active=*/true,
        static_cast<std::uint8_t>(generation & 0xFFu),
        BusId::Sfx);
    r.slots[slot_idx].packed.store(new_val, std::memory_order_release);
}

void mark_voice_dead(u32 slot_idx) noexcept {
    if (slot_idx >= kBusRouterMaxVoices) return;
    RouterState& r = router_state();
    // CAS-flip only the active bit; preserve gen + bus (so a re-play
    // that bumps gen can distinguish "dead with the same generation"
    // from "dead with an earlier generation").
    std::uint32_t old = r.slots[slot_idx].packed.load(std::memory_order_relaxed);
    std::uint32_t new_val;
    do {
        new_val = old & ~kSlotActiveBit;
    } while (!r.slots[slot_idx].packed.compare_exchange_weak(
        old, new_val,
        std::memory_order_release,
        std::memory_order_relaxed));
}

BusId slot_bus(u32 slot_idx) noexcept {
    // LOCK-FREE.  Called on the audio thread once per active voice every
    // pull — must not acquire a mutex.  Single acquire-load pairs with
    // the release-store in mark_voice_live / route_voice_to_bus.
    if (slot_idx >= kBusRouterMaxVoices) return BusId::Sfx;
    RouterState& r = router_state();
    const std::uint32_t v = r.slots[slot_idx].packed.load(std::memory_order_acquire);
    if (!unpack_active(v)) return BusId::Sfx;
    return unpack_bus(v);
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
    const std::uint8_t want_gen = static_cast<std::uint8_t>(gen & 0xFFu);
    // CAS loop — refuse to update if the slot has gone dead OR its
    // generation has bumped (a stop_voice → re-play race).  Lock-free
    // so the audio thread's slot_bus() never blocks on a UI thread
    // mid-route.
    std::uint32_t old = r.slots[idx].packed.load(std::memory_order_acquire);
    for (;;) {
        if (!detail::unpack_active(old))            return false;
        if (detail::unpack_gen(old) != want_gen)    return false;
        const std::uint32_t new_val = detail::pack_slot(true, want_gen, bus);
        if (r.slots[idx].packed.compare_exchange_weak(
                old, new_val,
                std::memory_order_release,
                std::memory_order_acquire)) {
            return true;
        }
        // CAS failed — `old` reloaded; retry.
    }
}

BusId voice_bus(VoiceId voice) noexcept {
    if (voice.raw == 0u) return BusId::Sfx;
    const u32 idx = detail::unpack_voice_index(voice.raw);
    const u32 gen = detail::unpack_voice_gen(voice.raw);
    if (idx >= detail::kBusRouterMaxVoices) return BusId::Sfx;

    detail::RouterState& r = detail::router_state();
    const std::uint32_t v =
        r.slots[idx].packed.load(std::memory_order_acquire);
    if (!detail::unpack_active(v)) return BusId::Sfx;
    if (detail::unpack_gen(v) != static_cast<std::uint8_t>(gen & 0xFFu)) {
        return BusId::Sfx;
    }
    return detail::unpack_bus(v);
}

}  // namespace psynder::audio
