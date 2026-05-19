// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/IUpscaler.h
//
// Lane 11 — Common interface for all vendor upscaler wrappers.
//
// Real SDK integration is M9 work (see PLAN.md §2 lane 11).
// Until then every concrete upscaler returns the source texture unmodified
// and set_upscaler_runtime(Upscaler::Off) is the only active code path.

#pragma once

#include "render/post/PublicRenderPost.h"
#include "gpu/PublicGpu.h"

namespace psynder::render::post::upscale {

// Passed by the host each frame; all fields are optional (may be null).
struct UpscaleInput
{
    gpu::Texture* color         = nullptr; // linear HDR, render resolution
    gpu::Texture* depth         = nullptr; // depth buffer, render resolution
    gpu::Texture* motion        = nullptr; // motion vectors, render resolution
    gpu::Texture* output        = nullptr; // output, display resolution
    float         render_scale  = 1.0f;    // render_res / display_res
    float         jitter_x      = 0.0f;   // sub-pixel jitter X (Halton)
    float         jitter_y      = 0.0f;   // sub-pixel jitter Y (Halton)
    bool          reset         = false;  // reset history (scene cut)
};

// Abstract upscaler interface. All concrete implementations live in this
// directory; PostProcess.cpp selects one at init() time.
class IUpscaler
{
public:
    virtual ~IUpscaler() = default;

    // True when the vendor SDK was found and the GPU is capable.
    // Always returns false in M2; real capability checks are M9 work.
    virtual bool is_supported() const noexcept { return false; }

    // Name for console / debug overlay.
    virtual const char* name() const noexcept = 0;

    // Evaluate one upscale frame. Host guarantees all input textures are
    // valid if is_supported() and active. Stub impl is a no-op.
    virtual void evaluate(const UpscaleInput&) {}
};

} // namespace psynder::render::post::upscale
