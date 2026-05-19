// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/metalfx.h
//
// Lane 11 — Apple MetalFX upscaler stub (macOS / Apple Silicon).
//
// MetalFX Spatial and Temporal upscaling (MTLFXSpatialScaler /
// MTLFXTemporalScaler) are part of the Metal framework on macOS 13+.
// Integration uses Metal-cpp or Obj-C++ (the .mm translation unit).
// Full integration is M9 work.
//
// This header is included on all platforms but the implementation is
// compiled only on macOS (guarded by PSYNDER_PLATFORM_MACOS).  On other
// platforms the stub always reports is_supported() == false.

#pragma once

#include "IUpscaler.h"

namespace psynder::render::post::upscale {

class MetalFx final : public IUpscaler
{
public:
    explicit MetalFx(gpu::Device* device);
    ~MetalFx() override;

    bool       is_supported() const noexcept override;
    const char* name()        const noexcept override { return "MetalFX"; }

    // M9 macOS: [scaler encodeToCommandBuffer:].  M2: no-op.
    void evaluate(const UpscaleInput&) override;

private:
    gpu::Device* device_ = nullptr;
    // M9 (macOS only): id<MTLFXTemporalScaler> scaler_ = nil;
};

} // namespace psynder::render::post::upscale
