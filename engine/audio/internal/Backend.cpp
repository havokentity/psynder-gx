// SPDX-License-Identifier: MIT
// Psynder — audio backend dispatcher + weak fallback implementations.
//
// The real WASAPI / CoreAudio / PipeWire / ALSA wiring lives in the platform
// lanes (21 win32, 22 linux, 23 macos). Until those land, this TU provides
// fallback implementations marked `[[gnu::weak]]` so any platform lane that
// supplies a strong symbol takes precedence at link time. Engine code calls
// the dispatcher entry points unconditionally, so Wave-A builds stay green
// regardless of which platform lane is merged.
//
// macOS AUHAL hook:
//   On macOS, backend_init_coreaudio uses lane 25's
//   psynder::platform::macos::default_audio_device_name() and
//   default_audio_sample_rate() to query the system default output device
//   before handing off to the real AUHAL implementation (supplied by lane 25
//   as a strong symbol override). If the strong symbol is absent the weak
//   fallback below logs the device name and returns a null device so the
//   mixer thread still runs at the correct sample rate.
//
// Win32 hook:
//   Lane 23 exposes psynder::platform::win32::audio_singleton() +
//   audio_set_mixer() in Win32Audio.h. The strong backend_init_wasapi
//   symbol is provided by lane 23 and routes through Win32Audio.
//
// Linux hook:
//   Lane 24 exposes psynder::platform::linux_platform::audio_device_name().
//   The strong backend_init_pipewire / backend_init_alsa symbols are provided
//   by lane 24.
//
// Cross-lane Issue: neither lane 24 nor lane 23 exposes a public
//   default_audio_sample_rate() equivalent to lane 25's API. Filed as
//   ISSUE: "lane 23/24 missing default_audio_sample_rate() for lane 14".

#include "Backend.h"
#include "core/Log.h"

// macOS: include lane 25 public header so we can read the device hints.
#if defined(PSYNDER_PLATFORM_MACOS)
#   include "platform/macos/PublicPlatformMacos.h"
#endif

namespace psynder::audio {

namespace {
// Track the last-init'd callback so a null-device thread can keep the mixer
// alive (drives Engine::active_voice_count assertions even with no real HW).
MixerCallback g_null_cb   = nullptr;
void*         g_null_user = nullptr;
u32           g_null_sr   = 48000;
u32           g_null_frames = 512;
}  // namespace

// ─── Weak per-backend fallbacks ──────────────────────────────────────────
//
// Platform lanes provide strong overrides; the GCC/Clang `[[gnu::weak]]`
// attribute lets the linker prefer them when present. On MSVC we currently
// rely on the platform lanes shipping their own object — fine for Wave A
// because the orchestrator runs on macOS.
#if defined(__clang__) || defined(__GNUC__)
#   define PSY_WEAK __attribute__((weak))
#else
#   define PSY_WEAK
#endif

PSY_WEAK bool backend_init_wasapi(const DeviceDesc&, MixerCallback cb, void* user) noexcept {
    PSY_LOG_INFO("[audio] backend_init_wasapi: lane 12 fallback (null device).");
    g_null_cb = cb; g_null_user = user;
    return true;
}
PSY_WEAK void backend_shutdown_wasapi() noexcept {
    g_null_cb = nullptr; g_null_user = nullptr;
}

PSY_WEAK bool backend_init_coreaudio(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept {
#if defined(PSYNDER_PLATFORM_MACOS)
    // Query lane 25 for the system default device and its native sample rate.
    // The strong symbol override (supplied by lane 25 / MacPlatform.cpp) will
    // use these to open the AUHAL render unit targeting the exact device.
    const char*       dev_name = psynder::platform::macos::default_audio_device_name();
    const std::uint32_t dev_sr = psynder::platform::macos::default_audio_sample_rate();
    PSY_LOG_INFO("[audio] backend_init_coreaudio (weak fallback): device='{}' native_sr={}",
                 dev_name ? dev_name : "<unknown>", dev_sr);
    // Propagate the native sample rate so the mixer is initialised at the
    // correct rate even if the caller passed 0 or a mismatched value.
    g_null_sr = (dev_sr > 0u) ? dev_sr
                              : (desc.sample_rate > 0u ? desc.sample_rate : 48000u);
#else
    PSY_LOG_INFO("[audio] backend_init_coreaudio: fallback (non-macOS build, null device).");
    g_null_sr = (desc.sample_rate > 0u) ? desc.sample_rate : 48000u;
#endif
    g_null_cb     = cb;
    g_null_user   = user;
    g_null_frames = (desc.buffer_frames > 0u) ? desc.buffer_frames : 512u;
    return true;
}
PSY_WEAK void backend_shutdown_coreaudio() noexcept {
    g_null_cb = nullptr; g_null_user = nullptr;
}

PSY_WEAK bool backend_init_pipewire(const DeviceDesc&, MixerCallback cb, void* user) noexcept {
    PSY_LOG_INFO("[audio] backend_init_pipewire: lane 12 fallback (null device).");
    g_null_cb = cb; g_null_user = user;
    return true;
}
PSY_WEAK void backend_shutdown_pipewire() noexcept {
    g_null_cb = nullptr; g_null_user = nullptr;
}

PSY_WEAK bool backend_init_alsa(const DeviceDesc&, MixerCallback cb, void* user) noexcept {
    PSY_LOG_INFO("[audio] backend_init_alsa: lane 12 fallback (null device).");
    g_null_cb = cb; g_null_user = user;
    return true;
}
PSY_WEAK void backend_shutdown_alsa() noexcept {
    g_null_cb = nullptr; g_null_user = nullptr;
}

// ─── Dispatcher ──────────────────────────────────────────────────────────
bool backend_init(const DeviceDesc& desc, MixerCallback cb, void* user, Backend& chosen) noexcept {
    Backend b = desc.backend;
    if (b == Backend::Auto) {
#if defined(PSYNDER_PLATFORM_MACOS)
        b = Backend::CoreAudio;
#elif defined(PSYNDER_PLATFORM_WIN32)
        b = Backend::WASAPI;
#elif defined(PSYNDER_PLATFORM_LINUX)
        b = Backend::PipeWire;  // PipeWire-over-ALSA preferred per DESIGN
#else
        b = Backend::ALSA;
#endif
    }
    chosen = b;
    switch (b) {
        case Backend::WASAPI:    return backend_init_wasapi   (desc, cb, user);
        case Backend::CoreAudio: return backend_init_coreaudio(desc, cb, user);
        case Backend::PipeWire:  return backend_init_pipewire (desc, cb, user);
        case Backend::ALSA:      return backend_init_alsa     (desc, cb, user);
        case Backend::Auto:      return false;  // unreachable
    }
    return false;
}

void backend_shutdown(Backend chosen) noexcept {
    switch (chosen) {
        case Backend::WASAPI:    backend_shutdown_wasapi();    break;
        case Backend::CoreAudio: backend_shutdown_coreaudio(); break;
        case Backend::PipeWire:  backend_shutdown_pipewire();  break;
        case Backend::ALSA:      backend_shutdown_alsa();      break;
        case Backend::Auto:                                    break;
    }
}

}  // namespace psynder::audio
