// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/rt/Stubs.cpp
//
// Lane 10 — Render-RT Wave B stubs.
//
// Full hardware-RT shadows (§7.4), DDGI probe grid (§7.5), and hybrid
// SSR+RT reflections (§7.6) ship at M5. Until then every entry-point is
// a no-op so the rest of the engine can link and run on M0–M2 hardware.
//
// device_supports_rt() delegates to psynder::gpu::device_supports_rt via a
// process-wide device hook set by the application before calling any RT
// entry-point. When the hook is null (e.g. unit-test builds) the function
// returns false, which causes lane 09 to fall back to CSM shadows.

#include "render/rt/PublicRenderRT.h"
#include "gpu/PublicGpu.h"

#include <cstdio>

namespace psynder::render::rt {

namespace {

// Process-wide device pointer — mirrors the same pattern used in lane 09
// (render/pipeline/Pipeline.cpp). Set once before any RT call via the
// internal set_device_hook() below. Not in the frozen public header;
// M5 will fold this into an RtDesc or a cross-lane device registry.
gpu::Device* g_device = nullptr;

} // namespace

// ─── Internal hook (not in PublicRenderRT.h) ──────────────────────────────
//
// Called by the application (or lane 09 bootstrap) to tell lane 10 which
// GPU device to query for RT capability.  M5 will replace this with a
// proper device-registry lookup.
void set_device_hook(gpu::Device* dev) {
    g_device = dev;
}

// ─── Public API ───────────────────────────────────────────────────────────

bool device_supports_rt() {
    // Delegate to lane 07's per-device query. Returns false when the hook
    // has not been set (headless / unit-test mode) or the device lacks
    // hardware RT capability (M-series < 3, AMD < RDNA2, NVIDIA < Turing).
    return gpu::device_supports_rt(g_device);
}

// ─── RT shadow integrator (§7.4) — M5 stub ────────────────────────────────

void shadows_init(const ShadowsDesc&) {
    // M5: allocate SBT, denoiser scratch, per-frame shadow ray buffers.
    // M0–M2: CSM fallback is owned by lane 09 (render-pipeline).
    std::fputs("[psy::render::rt] shadows_init: stub (M5 work)\n", stderr);
}

void shadows_shutdown() {
    // M5: free SBT + denoiser resources.
}

// ─── DDGI probe grid (§7.5) — M5 stub ────────────────────────────────────

void ddgi_init(const DdgiDesc&) {
    // M5: allocate probe irradiance/visibility atlas, per-probe ray buffer,
    //     probe relocation scratch, and the indirect-lighting constant buffer.
    std::fputs("[psy::render::rt] ddgi_init: stub (M5 work)\n", stderr);
}

void ddgi_shutdown() {
    // M5: release atlas + ray buffer resources.
}

void ddgi_update_subset() {
    // M5: trace rays for probes_per_frame probes, blend into the atlas,
    //     run relocation pass when a probe fails validity checks.
    // Called per frame by lane 09; safe to no-op until M5.
}

// ─── Hybrid SSR + RT reflections (§7.6) — M5 stub ────────────────────────

void reflections_init(const ReflectionsDesc&) {
    // M5: allocate hit-colour and ray-direction GBuffer passes, denoiser
    //     scratch, and the SSR ray-march constant buffer.
    std::fputs("[psy::render::rt] reflections_init: stub (M5 work)\n", stderr);
}

void reflections_shutdown() {
    // M5: release reflection GBuffer + denoiser resources.
}

} // namespace psynder::render::rt
