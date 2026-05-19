// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/xess.cpp — XeSS upscaler stub (M9 placeholder).

#include "xess.h"

namespace psynder::render::post::upscale {

Xess::Xess(gpu::Device* device) : device_(device)
{
    // M9: xessD3D12CreateContext() or xessVKCreateContext().
    // M9: xessSelectNetworkModel(), xessSetJitterScale().
    (void)device_;
}

Xess::~Xess()
{
    // M9: xessDestroyContext().
}

bool Xess::is_supported() const noexcept
{
    // M9: return xessIsOptimalDriver(context_) == XESS_RESULT_SUCCESS
    //     && at least DP4a is available.
    return false;
}

void Xess::evaluate(const UpscaleInput& /*input*/)
{
    // M9: Build xess_d3d12_execute_params_t / xess_vk_execute_params_t from
    //     UpscaleInput; call xessD3D12Execute() / xessVKExecute().
}

} // namespace psynder::render::post::upscale
