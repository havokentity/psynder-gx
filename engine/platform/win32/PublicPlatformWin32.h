// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/win32/PublicPlatformWin32.h
//
// Lane 23 — Win32 platform PUBLIC CONTRACT (Wave B).
//
// Mirrors the surface that lane 25 (PublicPlatformMacos.h) exposes so that
// lane 14 (audio) can open its WASAPI backend without per-OS ifdefs.
//
// This header is intentionally free of <windows.h> so other lanes can
// include it from plain .cpp translation units on any host OS.  The WASAPI /
// COM interop is fully hidden behind the .cpp in this directory.
//
// Threading: every entry point is callable from the main thread only.

#pragma once

#include <cstdint>

#if defined(PSYNDER_PLATFORM_WIN32)

namespace psynder::platform::win32 {

// ─── CoreAudio-equivalent device plumbing for lane 14 ────────────────────
// Returns the system default-render endpoint's friendly name (UTF-8).
// Uses MMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole, ...)
// + IMMDevice::OpenPropertyStore + PKEY_Device_FriendlyName.
//
// The pointer references an internal static buffer; it is valid until the
// next call.  Returns a non-null empty string on failure — never nullptr.
const char* default_audio_device_name();

// Returns the default render endpoint's mix-format sample rate (Hz) as
// reported by IAudioClient::GetMixFormat.  Returns 0 on failure.
// Lane 14 uses this as the device-format hint when initialising its
// WASAPI render client.
std::uint32_t default_audio_sample_rate();

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
