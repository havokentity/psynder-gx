// SPDX-License-Identifier: MIT
//
// engine/audio/internal/BusTestHook.h
//
// Lane 23 — test-only hooks into the BusMixer.
//
// This header exposes internal state of the audio bus mixer so the lane
// 25 (tests) unit suite can verify routing, gain ramps, LP filtering, and
// reverb sends without depending on an actual audio device. The header is
// LANE-INTERNAL — no game-side or other engine lane should ever include
// it; the public bus surface lives in `engine/audio/Bus.h`.
//
// Two access patterns:
//   1. `engine_bus_mixer()` — returns the global BusMixer owned by the
//      lane-23 router singleton in BusApi.cpp. Available without calling
//      Engine::start() because the router lives in its own TU and is
//      constructed lazily on first access.
//   2. Construct a standalone `BusMixer` and call its public API. This
//      is the recommended path for unit tests that want a clean DSP
//      slate and don't need to round-trip through the free functions.

#pragma once

#include "audio/BusMixer.h"

namespace psynder::audio::detail {

// Returns a pointer to the lane-23 router's global BusMixer instance.
// Non-owning; the lifetime is the program's. Always non-null on the
// first call (the router singleton constructs lazily).
//
// Tests can use this to inspect bus state after route_voice_to_bus(),
// set_bus_gains() calls — and to verify the integration hook between
// Audio.cpp's voice mixing loop and the bus graph (when also driving
// the BusRouter via mark_voice_live / mark_voice_dead).
BusMixer* engine_bus_mixer() noexcept;

}  // namespace psynder::audio::detail
