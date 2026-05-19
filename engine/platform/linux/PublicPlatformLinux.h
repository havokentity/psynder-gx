// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/linux/PublicPlatformLinux.h
//
// Lane 24 — Linux platform PUBLIC CONTRACT.
//
// Exposes:
//   1. Audio device enumeration — mirrors PublicPlatformMacos.h so lane 14
//      can query the default output sink without knowing the audio backend.
//   2. Native window handle accessor — gives lane 07 (gpu) a typed pointer
//      (psynder::gpu::LinuxNativeWindowHandle*) so it can dispatch between
//      vkCreateWaylandSurfaceKHR and vkCreateXcbSurfaceKHR.  Resolves
//      Issue lane09-002.
//
// Guard: only compiled when PSYNDER_PLATFORM_LINUX=1.
// Dedicated-server builds (PSYNDER_GX_DEDICATED_SERVER=1) strip all
// windowing and audio — do not call these in server-only code paths.
//
// Threading: callable from any thread; internals serialize with a mutex.
// Returned char* from default_audio_device_name() is a static buffer owned
// by the implementation; valid until the next call to that function.
//
// Audio backend priority (selected at compile time via CMake pkg_check_modules):
//   1. PipeWire   (libpipewire-0.3-dev)   — PSYNDER_LINUX_AUDIO_PIPEWIRE=1
//   2. PulseAudio (libpulse-dev)          — PSYNDER_LINUX_AUDIO_PULSE=1
//   3. ALSA stub  (always available)      — PSYNDER_LINUX_AUDIO_ALSA_STUB=1

#pragma once

#if PSYNDER_PLATFORM_LINUX
#ifndef PSYNDER_GX_DEDICATED_SERVER

#include <cstdint>

#include "gpu/PublicGpu.h"       // for psynder::gpu::LinuxNativeWindowHandle

namespace psynder::platform {
struct Window; // forward-decl from platform/Platform.h
} // namespace psynder::platform

namespace psynder::platform::linux_platform {

// ─── Native window handle ────────────────────────────────────────────────
//
// Returns a pointer to a psynder::gpu::LinuxNativeWindowHandle struct that
// was populated when the window was created.  The struct is owned by the
// window and remains valid for the window's lifetime.
//
// Lane 07 (gpu) calls this inside create_device() to obtain the typed handle
// and passes it — cast to void* — via DeviceDesc::native_window_handle.
// The Vulkan backend then casts it back to LinuxNativeWindowHandle* and
// dispatches to the right WSI extension.
//
// Returns nullptr if `win` is null or was created by the dedicated-server
// stub (which has no OS-level surface).
const psynder::gpu::LinuxNativeWindowHandle* native_window_handle(
    psynder::platform::Window* win);

// ─── Audio device enumeration ────────────────────────────────────────────
//
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
