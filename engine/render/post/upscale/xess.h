// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/xess.h
//
// Lane 11 — Intel XeSS (Xe Super Sampling) upscaler stub.
//
// XeSS SDK: https://github.com/intel/xess  (MIT license)
// Native quality on Intel Arc; cross-vendor DP4a fallback on other GPUs.
// Full integration is M9 work.
//
// At M2 this class always reports is_supported() == false.

#pragma once

#include "IUpscaler.h"

namespace psynder::render::post::upscale {

class Xess final : public IUpscaler
{
public:
    explicit Xess(gpu::Device* device);
    ~Xess() override;

    bool       is_supported() const noexcept override;
    const char* name()        const noexcept override { return "XeSS"; }

    // M9: xessD3D12Execute() / xessVKExecute(). M2: no-op.
    void evaluate(const UpscaleInput&) override;

private:
    gpu::Device* device_ = nullptr;
    // M9: xess_context_handle_t context_ {};
};

} // namespace psynder::render::post::upscale
