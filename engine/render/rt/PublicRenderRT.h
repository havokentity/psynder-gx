// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/rt/PublicRenderRT.h
//
// Lane 10 — Render RT public-header CONTRACT (Wave 0).
//
// Hardware RT shadows + DDGI probe grid + hybrid SSR/RT reflections.
// Full impl is M5 work; M0–M2 ship a no-op (CSM fallback in lane 09).
//
// See DESIGN-PSYNDER-GX.md §7.4–§7.6.

#pragma once

#include <cstdint>

namespace psynder::render::rt {

// Hardware-RT capability detected at startup. False on M-series < 3, AMD < RDNA2,
// NVIDIA < Turing. When false: CSM shadows (lane 09 fallback path), baked DDGI
// probes (offline via lm_bake_gx), SSR-only reflections.
bool device_supports_rt();

// RT shadow integrator. Implements §7.4. Called once per fragment in the
// forward+ opaque pass via shader binding table.
struct ShadowsDesc {
    bool enable_sun_shadow      = true;
    bool enable_light_shadows   = true; // dynamic-light packet shadows
    std::uint32_t denoiser_taps = 5;    // à-trous taps; 0 disables denoise
};
void shadows_init(const ShadowsDesc&);
void shadows_shutdown();

// DDGI probe grid. Implements §7.5. Updates a subset of probes per frame.
struct DdgiDesc {
    std::uint32_t grid_x = 16;
    std::uint32_t grid_y = 8;
    std::uint32_t grid_z = 16;
    float         probe_spacing_m = 2.0f; // 1 unit = 1 metre
    std::uint32_t rays_per_probe  = 64;
    std::uint32_t probes_per_frame = 256; // ~12% of grid per frame
};
void ddgi_init(const DdgiDesc&);
void ddgi_shutdown();
void ddgi_update_subset(); // called per frame from lane 09

// Hybrid SSR + RT reflections. Implements §7.6.
struct ReflectionsDesc {
    bool          enable_ssr = true;
    bool          enable_rt_fallback = true;
    std::uint32_t ssr_max_steps = 32;
};
void reflections_init(const ReflectionsDesc&);
void reflections_shutdown();

} // namespace psynder::render::rt
