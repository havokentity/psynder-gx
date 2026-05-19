// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/linux/LinuxAudio.cpp
//
// Lane 24 — Linux audio device accessors (default sink name + sample rate).
//
// Backend is chosen at compile time by CMake pkg_check_modules:
//   PSYNDER_LINUX_AUDIO_PIPEWIRE  — PipeWire  (preferred)
//   PSYNDER_LINUX_AUDIO_PULSE     — PulseAudio (fallback)
//   PSYNDER_LINUX_AUDIO_ALSA_STUB — hard-coded stub (last resort)
//
// Only compiled when PSYNDER_PLATFORM_LINUX=1 and
// PSYNDER_GX_DEDICATED_SERVER is NOT set.

#include "PublicPlatformLinux.h"

#if PSYNDER_PLATFORM_LINUX
#ifndef PSYNDER_GX_DEDICATED_SERVER

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
// Shared: static name buffer + lock
// ─────────────────────────────────────────────────────────────────────────────
namespace {
    constexpr std::size_t kNameBuf = 256;
    char    g_name_buf[kNameBuf] = "default";
    std::uint32_t g_sample_rate  = 48000u;
    std::mutex    g_mutex;
    bool          g_probed       = false;
} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Backend: PipeWire
// ─────────────────────────────────────────────────────────────────────────────
#if PSYNDER_LINUX_AUDIO_PIPEWIRE

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>

namespace {

struct PwQuery {
    bool done               = false;
    char name[kNameBuf]     = "default";
    std::uint32_t rate      = 48000u;
};

static void on_registry_global(void*        data,
                                std::uint32_t id,
                                std::uint32_t /*permissions*/,
                                const char*  type,
                                std::uint32_t /*version*/,
                                const struct spa_dict* props)
{
    // We only care about the first sink/audio-sink node.
    auto* q = static_cast<PwQuery*>(data);
    if (q->done) return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class) return;
    if (std::strcmp(media_class, "Audio/Sink") != 0) return;

    // Name / description
    const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!desc) desc = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!desc) desc = "default";
    std::snprintf(q->name, kNameBuf, "%s", desc);

    // Sample rate from node.rate if exposed as a param (best-effort).
    // The format param requires connecting a proxy — for simplicity we probe
    // the "node.param.Props" dict key "audioConvert.samplerateQuality" which
    // is sometimes present, then fall back to the PW_KEY_AUDIO_RATE prop.
    const char* rate_str = spa_dict_lookup(props, "audio.rate");
    if (rate_str) {
        unsigned long r = std::strtoul(rate_str, nullptr, 10);
        if (r > 0 && r <= 384000) {
            q->rate = static_cast<std::uint32_t>(r);
        }
    }

    q->done = true;
}

static const pw_registry_events kRegEvents = {
    PW_VERSION_REGISTRY_EVENTS,
    /*global*/          on_registry_global,
    /*global_remove*/   nullptr,
};

void probe_pipewire(char* out_name, std::uint32_t& out_rate) noexcept
{
    // pw_init is idempotent.
    int fake_argc = 0;
    char** fake_argv = nullptr;
    pw_init(&fake_argc, &fake_argv);

    struct pw_main_loop*   loop     = pw_main_loop_new(nullptr);
    if (!loop) return;

    struct pw_context*     context  = pw_context_new(pw_main_loop_get_loop(loop),
                                                     nullptr, 0);
    if (!context) { pw_main_loop_destroy(loop); return; }

    struct pw_core*        core     = pw_context_connect(context, nullptr, 0);
    if (!core) { pw_context_destroy(context); pw_main_loop_destroy(loop); return; }

    struct pw_registry*    registry = pw_core_get_registry(core,
                                                           PW_VERSION_REGISTRY, 0);
    PwQuery q;
    spa_hook registry_listener;
    pw_registry_add_listener(registry, &registry_listener, &kRegEvents, &q);

    // Sync once so the registry global callbacks fire.
    struct spa_hook core_listener;
    struct pw_core_events core_evs = { PW_VERSION_CORE_EVENTS };
    int pending = 0;
    struct SyncDone { bool hit; };
    SyncDone sd { false };

    auto on_done = [](void* data, std::uint32_t /*id*/, int /*seq*/) {
        auto* s = static_cast<SyncDone*>(data);
        s->hit = true;
    };
    core_evs.done = on_done;
    pw_core_add_listener(core, &core_listener, &core_evs, &sd);

    pending = pw_core_sync(core, PW_ID_CORE, 0);
    (void)pending;

    // Iterate the main-loop until the sync done fires or we time-out.
    // pw_main_loop_run would block forever; instead pump manually up to 50ms.
    for (int i = 0; i < 50 && !sd.hit; ++i) {
        // 1 ms worth of events
        pw_loop_iterate(pw_main_loop_get_loop(loop), 1);
    }

    if (q.done) {
        std::snprintf(out_name, kNameBuf, "%s", q.name);
        out_rate = q.rate;
    }

    spa_hook_remove(&registry_listener);
    spa_hook_remove(&core_listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    pw_deinit();
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Backend: PulseAudio
// ─────────────────────────────────────────────────────────────────────────────
#elif PSYNDER_LINUX_AUDIO_PULSE

#include <pulse/pulseaudio.h>

namespace {

struct PaQuery {
    bool          done    = false;
    char          name[kNameBuf] = "default";
    std::uint32_t rate    = 48000u;
};

static void pa_sink_info_cb(pa_context* /*ctx*/,
                             const pa_sink_info* info,
                             int eol,
                             void* userdata)
{
    auto* q = static_cast<PaQuery*>(userdata);
    if (eol || !info || q->done) return;

    const char* desc = info->description;
    if (!desc || desc[0] == '\0') desc = info->name;
    if (!desc) desc = "default";
    std::snprintf(q->name, kNameBuf, "%s", desc);
    q->rate = info->sample_spec.rate;
    q->done = true;
}

static void pa_server_info_cb(pa_context* ctx,
                               const pa_server_info* info,
                               void* userdata)
{
    if (!info) return;
    auto* q = static_cast<PaQuery*>(userdata);
    if (info->default_sink_name && info->default_sink_name[0] != '\0') {
        pa_context_get_sink_info_by_name(ctx, info->default_sink_name,
                                        pa_sink_info_cb, userdata);
    }
}

static void pa_state_cb(pa_context* ctx, void* userdata)
{
    // nothing — mainloop drives us
    (void)ctx; (void)userdata;
}

void probe_pulseaudio(char* out_name, std::uint32_t& out_rate) noexcept
{
    pa_mainloop*     ml  = pa_mainloop_new();
    if (!ml) return;
    pa_mainloop_api* api = pa_mainloop_get_api(ml);

    pa_context* ctx = pa_context_new(api, "psynder-gx-probe");
    if (!ctx) { pa_mainloop_free(ml); return; }

    pa_context_set_state_callback(ctx, pa_state_cb, nullptr);
    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(ctx);
        pa_mainloop_free(ml);
        return;
    }

    PaQuery q;
    bool server_queried = false;

    // Pump loop: wait for READY, send server-info query, wait for result.
    for (int iter = 0; iter < 200; ++iter) {
        pa_mainloop_iterate(ml, 0, nullptr);

        pa_context_state_t state = pa_context_get_state(ctx);
        if (state == PA_CONTEXT_READY && !server_queried) {
            pa_context_get_server_info(ctx, pa_server_info_cb, &q);
            server_queried = true;
        }
        if (q.done) break;
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) break;
    }

    if (q.done) {
        std::snprintf(out_name, kNameBuf, "%s", q.name);
        out_rate = q.rate;
    }

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Backend: ALSA stub (no external lib required)
// ─────────────────────────────────────────────────────────────────────────────
#else // PSYNDER_LINUX_AUDIO_ALSA_STUB

namespace {
void probe_alsa_stub(char* out_name, std::uint32_t& out_rate) noexcept
{
    std::snprintf(out_name, kNameBuf, "default");
    out_rate = 48000u;
}
} // anonymous namespace

#endif // backend selection

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
namespace psynder::platform::linux_platform {

static void ensure_probed()
{
    if (g_probed) return;

#if PSYNDER_LINUX_AUDIO_PIPEWIRE
    probe_pipewire(g_name_buf, g_sample_rate);
#elif PSYNDER_LINUX_AUDIO_PULSE
    probe_pulseaudio(g_name_buf, g_sample_rate);
#else
    probe_alsa_stub(g_name_buf, g_sample_rate);
#endif

    g_probed = true;
}

const char* default_audio_device_name()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ensure_probed();
    return g_name_buf;
}

std::uint32_t default_audio_sample_rate()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    ensure_probed();
    return g_sample_rate;
}

} // namespace psynder::platform::linux_platform

#endif // !PSYNDER_GX_DEDICATED_SERVER
#endif // PSYNDER_PLATFORM_LINUX
