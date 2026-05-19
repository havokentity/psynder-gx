// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/Hiz.cpp
//
// Lane 09 — depth pre-pass + HiZ pyramid (compute downsample).
//
// DESIGN-PSYNDER-GX.md §7.1 steps 3 + 5:
//   - Depth pre-pass renders the opaque set into a Depth32Float texture
//     with no color attachment. Provides early-Z for the forward+
//     opaque pass and a base for the HiZ pyramid.
//   - Compute downsample chain builds a mip pyramid where each mip
//     stores the conservative-max depth of the 2×2 source region (used
//     by the GPU cull compute pass for occlusion testing).
//
// Wave-B scope:
//   - Allocate the depth texture sized to PipelineDesc::internal_*.
//   - Allocate the HiZ pyramid (R32Uint, mips down to 1×1).
//   - Create the downsample compute pipeline via lane 08.
//   - encode_depth_prepass: log + drop until cmd encoder APIs land.
//   - encode_hiz_downsample: log + drop. See INTEGRATION (Issue lane09-001).

#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace psynder::render::pipeline {

namespace {

std::uint32_t mip_count_for(std::uint32_t w, std::uint32_t h) {
    std::uint32_t m = 1;
    std::uint32_t d = std::max(w, h);
    while (d > 1u) { d >>= 1u; ++m; }
    return m;
}

} // namespace

bool init_hiz_pyramid(Pipeline* p) {
    if (!p) return false;

    const std::uint32_t w = p->desc.internal_width;
    const std::uint32_t h = p->desc.internal_height;
    if (w == 0 || h == 0) {
        std::fputs("[psy::render::pipeline] init_hiz_pyramid: zero-size internal RT\n", stderr);
        return false;
    }

    p->hiz.mip_count = mip_count_for(w, h);

    if (p->device) {
        gpu::TextureDesc depth_desc{};
        depth_desc.width      = w;
        depth_desc.height     = h;
        depth_desc.format     = gpu::Format::Depth32Float;
        depth_desc.usage      = static_cast<std::uint32_t>(gpu::TextureUsage::DepthStencil)
                              | static_cast<std::uint32_t>(gpu::TextureUsage::Sampled);
        depth_desc.heap       = gpu::HeapKind::Transient;
        depth_desc.debug_name = "depth_prepass";
        p->hiz.depth_texture = gpu::create_texture(p->device, depth_desc);

        gpu::TextureDesc hiz_desc{};
        hiz_desc.width      = w;
        hiz_desc.height     = h;
        hiz_desc.mips       = p->hiz.mip_count;
        hiz_desc.format     = gpu::Format::R32Uint;
        hiz_desc.usage      = static_cast<std::uint32_t>(gpu::TextureUsage::Storage)
                            | static_cast<std::uint32_t>(gpu::TextureUsage::Sampled);
        hiz_desc.heap       = gpu::HeapKind::DeviceLocal;
        hiz_desc.debug_name = "hiz_pyramid";
        p->hiz.hiz_pyramid = gpu::create_texture(p->device, hiz_desc);

        if (!p->hiz.depth_texture || !p->hiz.hiz_pyramid) {
            std::fputs("[psy::render::pipeline] init_hiz_pyramid: texture alloc failed\n", stderr);
            return false;
        }
    }

    shader::ComputePipelineDesc cdesc{};
    cdesc.slang_path     = shader_path("hiz_build.slang");
    cdesc.entry_point_cs = "cs_downsample";
    p->hiz.downsample_compute = shader::create_compute(cdesc);
    if (!p->hiz.downsample_compute.valid()) {
        std::fputs("[psy::render::pipeline] init_hiz_pyramid: HiZ compile failed; "
                   "pass will be skipped\n", stderr);
    }

    std::printf("[psy::render::pipeline] hiz: depth=%ux%u  pyramid_mips=%u  pipeline=%s\n",
                w, h, p->hiz.mip_count,
                p->hiz.downsample_compute.valid() ? "ok" : "missing");
    return true;
}

void encode_depth_prepass(Pipeline* p,
                          gpu::CmdBuffer* /*cmd*/,
                          const View& /*view*/) {
    if (!p) return;
    // Real encode: begin a depth-only render pass with depth_texture,
    // bind the opaque pipeline (depth-only variant), iterate Renderables
    // and draw indexed. Blocked on lane 07 cmd encoder APIs. See lane09-001.
    if (!p->warned_missing_cmd_encoder && !p->renderables.empty()) {
        std::printf("[psy::render::pipeline] depth_prepass would draw %zu instances\n",
                    p->renderables.size());
    }
}

void encode_hiz_downsample(Pipeline* p, gpu::CmdBuffer* /*cmd*/) {
    if (!p || p->hiz.mip_count <= 1) return;
    // Real encode: bind hiz_pyramid + depth_texture, dispatch one compute
    // group per 8×8 tile per mip level, writing the conservative max into
    // each successive mip. Blocked on lane 07 cmd encoder APIs.
    if (!p->warned_missing_cmd_encoder) {
        std::printf("[psy::render::pipeline] hiz_downsample would chain %u mip dispatches\n",
                    p->hiz.mip_count - 1u);
    }
}

} // namespace psynder::render::pipeline
