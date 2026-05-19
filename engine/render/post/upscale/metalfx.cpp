// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/metalfx.cpp — MetalFX upscaler stub (M9 placeholder).
//
// NOTE: At M9 this file will become metalfx.mm (Obj-C++ / Metal-cpp).
// For M2 it is a plain .cpp stub so it builds on all platforms without
// macOS SDK dependencies.

#include "metalfx.h"

namespace psynder::render::post::upscale {

MetalFx::MetalFx(gpu::Device* device) : device_(device)
{
    // M9 (macOS 13+): Create MTLFXTemporalScalerDescriptor, configure
    // input/output pixel formats, call newTemporalScalerWithDescriptor:.
    // M9: Also detect Apple Silicon unified memory for quality budget.
    (void)device_;
}

MetalFx::~MetalFx()
{
    // M9: Release the MTLFXTemporalScaler (ARC handles Obj-C retain cycle).
}

bool MetalFx::is_supported() const noexcept
{
#if defined(PSYNDER_PLATFORM_MACOS)
    // M9: return [MTLFXTemporalScalerDescriptor supportsDevice:mtl_device_]
    //     && os_version >= macOS 13.
    return false; // placeholder until M9
#else
    return false;
#endif
}

void MetalFx::evaluate(const UpscaleInput& /*input*/)
{
    // M9 (macOS): Build MTLFXTemporalScalerDescriptor input params from
    // UpscaleInput, call [scaler encodeToCommandBuffer:] on the active
    // Metal command buffer provided by psy::gpu::CmdBuffer.
}

} // namespace psynder::render::post::upscale
