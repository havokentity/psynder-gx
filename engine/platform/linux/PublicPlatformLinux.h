// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/linux/PublicPlatformLinux.h
//
// Lane 24 — Linux platform PUBLIC CONTRACT for audio device enumeration.
//
// Mirrors the CoreAudio accessors in PublicPlatformMacos.h so lane 14
// (audio) can query the default output sink on Linux without knowing
// whether PipeWire, PulseAudio, or ALSA is in use at runtime.
//
// Guard: only compiled when PSYNDER_PLATFORM_LINUX=1.
// Dedicated-server builds (PSYNDER_GX_DEDICATED_SERVER=1) strip all audio
// entirely — do not include or call these in server-only code paths.
//
// Threading: callable from any thread; internals serialize with a mutex.
// Returned char* is a static buffer owned by the lane implementation;
// valid until the next call to default_audio_device_name().
//
// Backend priority (selected at compile time via CMake pkg_check_modules):
//   1. PipeWire   (libpipewire-0.3-dev)   — PSYNDER_LINUX_AUDIO_PIPEWIRE=1
//   2. PulseAudio (libpulse-dev)          — PSYNDER_LINUX_AUDIO_PULSE=1
//   3. ALSA stub  (always available)      — PSYNDER_LINUX_AUDIO_ALSA_STUB=1
//
// Lane 14 reads these values once during audio context initialisation and
// passes them to its mixer / device-open path.

#pragma once

#if PSYNDER_PLATFORM_LINUX
#ifndef PSYNDER_GX_DEDICATED_SERVER

#include <cstdint>

namespace psynder::platform::linux_platform {

// Returns the human-readable name of the system default audio output sink
// (UTF-8). On PipeWire this is the description of the default sink node; on
// PulseAudio it is the description field from pa_sink_info; on the ALSA stub
// it returns "default".
//
// The pointer references an internal static buffer. Copy the string if you
// need to keep it across frames or calls.
const char* default_audio_device_name();

// Returns the sample rate (Hz) reported by the default audio output sink.
// Typical values: 44100, 48000, 96000. Falls back to 48000 if the backend
// cannot be queried (library not found at runtime, permission error, etc.).
std::uint32_t default_audio_sample_rate();

} // namespace psynder::platform::linux_platform

#endif // !PSYNDER_GX_DEDICATED_SERVER
#endif // PSYNDER_PLATFORM_LINUX
