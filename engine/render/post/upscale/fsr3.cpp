// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/fsr3.cpp — FSR 3 upscaler stub (M9 placeholder).

#include "fsr3.h"

namespace psynder::render::post::upscale {

Fsr3::Fsr3(gpu::Device* device) : device_(device)
{
    // M9: ffxFsr3ContextCreate() with VK / DX12 backend interface.
    // M9: Set quality mode from r_upscaler_quality cvar.
    (void)device_;
}

Fsr3::~Fsr3()
{
    // M9: ffxFsr3ContextDestroy().
}

bool Fsr3::is_supported() const noexcept
{
    // FSR 3 has no GPU capability gate — it runs on any GPU via shaders.
    // M9: return true here unconditionally (or after context init succeeded).
    return false;
}

void Fsr3::evaluate(const UpscaleInput& /*input*/)
{
    // M9: Build FfxFsr3DispatchUpscaleDescription from UpscaleInput,
    //     call ffxFsr3ContextDispatchUpscale().
}

} // namespace psynder::render::post::upscale
