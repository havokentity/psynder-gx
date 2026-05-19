// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/fsr3.h
//
// Lane 11 — AMD FSR 3 (FidelityFX Super Resolution) upscaler stub.
//
// FSR 3 is cross-vendor and open-source (MIT).  The vendor SDK is available
// from https://gpuopen.com/fidelityfx-superresolution-3/.
// Full integration is M9 work.
//
// At M2 this class always reports is_supported() == false.

#pragma once

#include "IUpscaler.h"

namespace psynder::render::post::upscale {

class Fsr3 final : public IUpscaler
{
public:
    explicit Fsr3(gpu::Device* device);
    ~Fsr3() override;

    bool       is_supported() const noexcept override;
    const char* name()        const noexcept override { return "FSR3"; }

    // M9: ffxFsr3ContextDispatch(). M2: no-op.
    void evaluate(const UpscaleInput&) override;

private:
    gpu::Device* device_ = nullptr;
    // M9: FfxFsr3Context context_ {};
};

} // namespace psynder::render::post::upscale
