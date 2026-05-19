// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/dlss.cpp — DLSS upscaler stub (M9 placeholder).

#include "dlss.h"

namespace psynder::render::post::upscale {

Dlss::Dlss(gpu::Device* device) : device_(device)
{
    // M9: NVSDK_NGX_D3D12_Init / NVSDK_NGX_VULKAN_Init / (no Metal path).
    // M9: Query DLSS feature support, create the DLSS feature handle.
    // M2: intentionally empty — DLSS is always unsupported at this milestone.
    (void)device_;
}

Dlss::~Dlss()
{
    // M9: NVSDK_NGX_VULKAN_DestroyParameters / ReleaseFeature.
}

bool Dlss::is_supported() const noexcept
{
    // M9: return true iff NGX init succeeded and DLSS feature is available.
    return false;
}

void Dlss::evaluate(const UpscaleInput& /*input*/)
{
    // M9: build NGX_DLSS_EvaluateFeature() param block, dispatch via
    //     the Vulkan cmd buffer provided by the host.
}

} // namespace psynder::render::post::upscale
