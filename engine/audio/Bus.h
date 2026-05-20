// SPDX-License-Identifier: MIT
//
// engine/audio/Bus.h
//
// Lane 23 — submix-graph bus IDs and per-bus parameter API.
//
// Voices created by Engine::play() default to BusId::Sfx. The engine then
// mixes each voice into its assigned bus accumulator, applies per-bus gain +
// 1-pole low-pass + reverb send, and sums the buses into the master device
// output. Audio.h's public surface stays frozen; the bus layer is wired in
// transparently inside the audio callback.
//
// This header is LANE-INTERNAL — the public bus surface (PublicAudio.h) is
// pulled in by the orchestrator at M3 once the API stabilises. Game-side
// callers using engine/audio/Audio.h won't see this header.
//
// Threading contract:
//   * `set_bus_gains` / `get_bus_gains` / `route_voice_to_bus` are callable
//     from any thread. They serialise against the audio callback via a
//     `std::mutex` that the callback `try_lock`s — on contention the
//     callback uses the last-frame snapshot (no priority inversion of the
//     real-time thread). See BusMixer.cpp for the canonical pattern.
//   * The audio callback itself is single-threaded by OS contract
//     (AURemoteIO / WASAPI event-driven / PipeWire RT thread). The bus
//     mixing pass is NOT fanned across the job system — context-switch
//     cost would blow the audio deadline.
//
// Units (real metric — DESIGN.md §3.5):
//   * gain_linear is a linear ratio (1.0 = unity, 0.5 = -6 dB, 2.0 = +6 dB).
//     NOT decibels.
//   * lp_hz is the 1-pole low-pass cutoff in hertz. 22000.0f (default) is
//     above the audible band at 48 kHz sample rate and is treated as
//     "bypass" by the mixer.
//   * reverb_send is a unit-less linear ratio in [0, 1] of the bus signal
//     copied to the global reverb (FDN) input.

#pragma once

#include "audio/Audio.h"
#include "core/Types.h"

#include <cstdint>

namespace psynder::audio {

// ─── Bus identifiers ─────────────────────────────────────────────────────
//
// Game-side custom buses start at 5 (Ambient + 1 ... 15). The fixed-stem
// IDs (Master/Sfx/Music/Voice/Ambient) are exposed first because they're
// the ones every shipping game uses; gameplay-specific custom buses
// (footsteps-bus, weapons-bus, dialogue-bus) can claim 5..15.
enum class BusId : std::uint8_t {
    Master  = 0,
    Sfx     = 1,
    Music   = 2,
    Voice   = 3,
    Ambient = 4,
    // 5..15 available for game-side custom buses.
};

// Maximum number of buses supported by the submix graph. Sized to fit in
// `std::uint8_t` and into a single cache line for BusState arrays.
inline constexpr std::uint32_t kMaxBuses = 16;

// Per-bus parameters. Plain-old-data — no padding-sensitive layout, no
// vtables, no constructors that require destruction. Safe to copy from any
// thread; the audio callback grabs a snapshot under the bus mutex.
//
// gain_linear   — linear voltage ratio (1.0 = unity). Range clamped to
//                 [0.0, 4.0] internally so the soft-clip stage downstream
//                 still has headroom against accidental overshoot.
// lp_hz         — 1-pole low-pass cutoff (Hz). >= 22000 = bypass.
// reverb_send   — unit-less ratio in [0, 1] copied to the global reverb
//                 input pre-FDN.
struct BusGains {
    float gain_linear = 1.0f;
    float lp_hz       = 22000.0f;
    float reverb_send = 0.0f;
};

// Set the current target gains for a bus. The audio callback ramps the
// applied gains toward the target over kRampSamples (see BusMixer.h) so
// the change is glitch-free. Out-of-range BusId (>= kMaxBuses) is a no-op.
//
// Thread-safe — uses a try_lock pattern on the audio thread, so this call
// from gameplay/UI threads never blocks the audio callback.
void set_bus_gains(BusId bus, const BusGains& g) noexcept;

// Read the most recently-set target gains for a bus. Returns a defaulted
// BusGains for out-of-range BusId. Thread-safe (mutex-guarded read).
BusGains get_bus_gains(BusId bus) noexcept;

// Per-voice routing. Voices created via Engine::play() default to
// BusId::Sfx; this setter overrides per-voice. Returns false if the
// voice is no longer alive (stale generation or never opened) or if
// `bus` is out of range.
//
// Thread-safe — serialised against the voice pool's existing mutex.
bool route_voice_to_bus(VoiceId voice, BusId bus) noexcept;

// Read the bus a voice is currently routed to. Returns BusId::Sfx (the
// default) for unknown voices. Thread-safe.
BusId voice_bus(VoiceId voice) noexcept;

}  // namespace psynder::audio
