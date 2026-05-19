// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/dlss.h
//
// Lane 11 — NVIDIA DLSS 3 upscaler stub.
//
// Real integration requires the DLSS SDK (NGX headers + SDK DLLs / .dylib),
// which is an NVIDIA NDA deliverable.  Full implementation is M9 work.
//
// At M2 this class always reports is_supported() == false and evaluate() is
// a no-op.  The host selects this path only when the user sets
//   r_upscaler dlss
// on an RTX 20+ GPU.  On unsupported GPUs the console logs a warning and
// falls back to Upscaler::Off.

#pragma once

#include "IUpscaler.h"

namespace psynder::render::post::upscale {

class Dlss final : public IUpscaler
{
public:
    // Constructs the wrapper.  On M2: no-op.
    // M9: initialise NGX SDK, query DLSS feature availability.
    explicit Dlss(gpu::Device* device);
    ~Dlss() override;

    bool       is_supported() const noexcept override;
    const char* name()        const noexcept override { return "DLSS"; }

    // M9: NGX_DLSS_EvaluateFeature(). M2: no-op.
    void evaluate(const UpscaleInput&) override;

private:
    gpu::Device* device_ = nullptr;
    // M9: NVSDK_NGX_Handle* feature_ = nullptr;
};

} // namespace psynder::render::post::upscale
