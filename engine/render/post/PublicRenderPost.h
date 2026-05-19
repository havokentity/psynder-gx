// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/PublicRenderPost.h
//
// Lane 11 — Render post-process public-header CONTRACT (Wave 0).
//
// Bloom, ACES tonemap, vendor-upscaler integration (DLSS/FSR/XeSS/MetalFX),
// final UI compositing pass.
//
// See DESIGN-PSYNDER-GX.md §7.8 for upscale, §10.6 for UI compositing.

#pragma once

#include <cstdint>

namespace psynder::render::post {

// Upscaler selector. Resolved at startup from detected GPU; user-overridable
// via console var `r_upscaler`.
enum class Upscaler : std::uint8_t {
    Off,
    Dlss,      // NVIDIA RTX 20+
    Fsr3,      // AMD FSR 3 (cross-vendor)
    Xess,      // Intel XeSS
    MetalFx,   // Apple Silicon
};

struct PostDesc {
    std::uint32_t output_width  = 1920;
    std::uint32_t output_height = 1080;
    Upscaler      upscaler      = Upscaler::Off;
    float         bloom_strength = 0.04f;
    float         exposure_ev    = 0.0f;
};

// Lifecycle.
void init(const PostDesc&);
void shutdown();

// Per-frame entry: consume the linear-HDR scene color attachment, produce
// the upscaled + tonemapped + bloomed + UI-composited backbuffer.
void run_frame();

// Console-var hook. Lane 01 (core) console reflects this.
void set_upscaler_runtime(Upscaler);

} // namespace psynder::render::post
