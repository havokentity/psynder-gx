// SPDX-License-Identifier: MIT
//
// engine/audio/internal/BusRouter.h
//
// Lane 23 — global bus state singleton interface (LANE-INTERNAL).
//
// The BusRouter owns:
//   * One BusMixer for the engine's submix graph.
//   * A per-voice-slot bus assignment array (kMaxVoices entries).
//   * A per-voice-slot liveness shadow (active flag + generation byte)
//     so route_voice_to_bus() can reject stale ids without touching
//     Audio.cpp's voice pool — keeping Audio.cpp.o and BusApi.cpp.o
//     linkage-independent.
//
// Audio.cpp's mixer_pull() and Engine::play()/stop_voice() forward into
// this singleton via the entry points below. Tests can drive the same
// entry points directly to exercise routing semantics without starting
// the audio engine (which would transitively require the platform lane's
// CoreAudio glue at link time).

#pragma once

#include "audio/Audio.h"   // VoiceId
#include "audio/Bus.h"     // BusId
#include "audio/BusMixer.h"
#include "core/Types.h"

#include <cstdint>

namespace psynder::audio::detail {

// Maximum voice slots tracked by the bus router. Matches MixerCore.h's
// kMaxVoices (the voice pool's capacity); duplicated as a literal here
// so this header doesn't pull in MixerCore.h's heavy stack of HRTF /
// FFT definitions.
inline constexpr std::uint32_t kBusRouterMaxVoices = 32u;

// Returns the global BusMixer instance. Always non-null.
BusMixer& bus_mixer() noexcept;

// Reset the router to its default state: all voice slots dead, all
// voices routed to Sfx, all bus gains at unity. Called by Engine::start()
// (via the integration hook in Audio.cpp) and Engine::stop(). Tests call
// this between scenarios for a clean slate.
void router_reset() noexcept;

// Initialise the BusMixer for a given sample rate / max frames. Called
// from Engine::start(). Tests can call directly when running standalone.
// Returns the underlying BusMixer::init() result.
bool router_init_mixer(u32 sample_rate, u32 max_frames) noexcept;

// Voice-slot liveness hooks. Audio.cpp calls these from Engine::play()
// and Engine::stop_voice() to keep the router's view of the voice pool
// in sync. Tests can drive them directly.
//
// `mark_voice_live` resets the bus assignment for the slot back to the
// SFX default (matching DESIGN §10.2 — every new voice starts on Sfx
// unless the caller immediately routes it elsewhere).
void mark_voice_live(u32 slot_idx, u32 generation) noexcept;
void mark_voice_dead(u32 slot_idx) noexcept;

// Returns the BusId currently assigned to slot `slot_idx`. The caller is
// responsible for ensuring `slot_idx` is for a live voice — Audio.cpp's
// mixer_pull already filters on voice activity before calling here.
// Out-of-range slots return Sfx.
BusId slot_bus(u32 slot_idx) noexcept;

}  // namespace psynder::audio::detail
