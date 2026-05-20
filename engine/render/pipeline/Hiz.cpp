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
// Wave-C update — encode_depth_prepass + encode_hiz_downsample now issue
// real begin_render / draw_indexed / dispatch via PublicGpu.h. Depth pre-
// pass uses the depth_only.slang shader (VS-only, no color writes); HiZ
// uses hiz_build.slang (one dispatch per mip).

#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace psynder::render::pipeline {

namespace {

constexpr std::uint32_t kHizTileSize = 8; // matches hiz_build.slang [numthreads(8,8,1)]

// Mirrors depth_only.slang's [[vk::push_constant]] block:
//   float4x4 view_proj
//   float4x4 world_matrix
// Total: 128 bytes — exactly fits kMaxPushConstantBytes.
struct DepthPushConstants {
    math::Mat4 view_proj;
    math::Mat4 world_matrix;
};
static_assert(sizeof(DepthPushConstants) == 128,
              "DepthPushConstants must equal kMaxPushConstantBytes");

// Mirrors hiz_build.slang's HizParams cbuffer.
struct HizPushConstants {
    std::uint32_t src_mip;
    std::uint32_t dst_mip;
    std::uint32_t src_width;
    std::uint32_t src_height;
};

std::uint32_t mip_count_for(std::uint32_t w, std::uint32_t h) {
    std::uint32_t m = 1;
    std::uint32_t d = std::max(w, h);
    while (d > 1u) { d >>= 1u; ++m; }
    return m;
}

// Compose a 4x4 = view * proj for the push-constant block. The View struct
// stores them separately and column-major; downstream shaders also expect
// column-major (Slang default), so a per-element copy is the cheapest path.
math::Mat4 compute_view_proj(const View& view) {
    return math::mul(view.proj_matrix, view.view_matrix);
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

    // Compile the HiZ downsample compute (one shader, dispatched per mip).
    shader::ComputePipelineDesc cdesc{};
    cdesc.slang_path     = shader_path("hiz_build.slang");
    cdesc.entry_point_cs = "cs_downsample";
    p->hiz.downsample_compute = shader::create_compute(cdesc);
    if (!p->hiz.downsample_compute.valid()) {
        std::fputs("[psy::render::pipeline] init_hiz_pyramid: HiZ compile failed; "
                   "pass will be skipped\n", stderr);
    }

    // Compile the depth-only graphics PSO for the pre-pass. Depth-write
    // enabled, no color attachment, matches the same vertex layout as
    // forward.slang (POSITION/NORMAL/UV/COLOR).
    shader::GraphicsPipelineDesc dpdesc{};
    dpdesc.slang_path         = shader_path("depth_only.slang");
    dpdesc.entry_point_vs     = "vs_main";
    dpdesc.entry_point_fs     = "fs_main";
    dpdesc.color_format_count = 0;
    dpdesc.depth_format       = static_cast<std::uint8_t>(gpu::Format::Depth32Float);
    dpdesc.enable_depth_write = true;
    dpdesc.enable_blend       = false;
    p->hiz.depth_only_graphics = shader::create_graphics(dpdesc);
    if (!p->hiz.depth_only_graphics.valid()) {
        std::fputs("[psy::render::pipeline] init_hiz_pyramid: depth_only compile failed; "
                   "pre-pass will be skipped\n", stderr);
    }

    std::printf("[psy::render::pipeline] hiz: depth=%ux%u  pyramid_mips=%u  "
                "downsample=%s  depth_only=%s\n",
                w, h, p->hiz.mip_count,
                p->hiz.downsample_compute.valid() ? "ok" : "missing",
                p->hiz.depth_only_graphics.valid() ? "ok" : "missing");
    return true;
}

void encode_depth_prepass(Pipeline* p,
                          gpu::CmdBuffer* cmd,
                          const View& view) {
    if (!p || !cmd) return;
    if (p->renderables.empty()) return;
    if (!p->hiz.depth_only_graphics.valid()) return;

    // Begin a depth-only render pass. depth.target == nullptr in headless
    // mode (no allocated depth_texture) would push us into a "no depth
    // attachment" path; the Metal backend declines to bind a depth
    // attachment and we still get a render-encoder scope so the draw_indexed
    // calls land. swapchain=false keeps the M0 auto-clear from triggering
    // and avoids racing the opaque pass for the drawable image.
    gpu::RenderPassDesc rpd{};
    rpd.swapchain    = false;
    rpd.color_count  = 0;
    rpd.depth.target = p->hiz.depth_texture.get();
    rpd.depth.load   = gpu::LoadOp::Clear;
    rpd.depth.store  = gpu::StoreOp::Store;
    rpd.depth.clear_depth   = 1.0f;
    rpd.depth.clear_stencil = 0;

    gpu::begin_render(cmd, rpd);
    ++p->stats.depth_passes;

    gpu::Viewport vp{
        0.0f, 0.0f,
        static_cast<float>(p->desc.internal_width),
        static_cast<float>(p->desc.internal_height),
        0.0f, 1.0f
    };
    gpu::set_viewport(cmd, vp);
    gpu::Scissor sc{
        0, 0,
        p->desc.internal_width,
        p->desc.internal_height
    };
    gpu::set_scissor(cmd, sc);

    gpu::bind_pipeline(cmd, p->hiz.depth_only_graphics);

    // DOTS: walk Renderables as a flat array, no per-entity vtable.
    const math::Mat4 view_proj = compute_view_proj(view);
    for (const Renderable& r : p->renderables) {
        if (!r.vertex_buffer || !r.index_buffer) continue;
        gpu::bind_vertex_buffer(cmd, 0, r.vertex_buffer);
        gpu::bind_index_buffer (cmd, r.index_buffer, gpu::IndexType::U16);

        DepthPushConstants pc{};
        pc.view_proj    = view_proj;
        pc.world_matrix = r.world_matrix;
        gpu::push_constants(cmd, &pc, sizeof(pc), gpu::ShaderStage::Vertex);

        gpu::draw_indexed(cmd, r.index_count);
        ++p->stats.depth_prepass_draws;
    }

    gpu::end_render(cmd);
}

void encode_hiz_downsample(Pipeline* p, gpu::CmdBuffer* cmd) {
    if (!p || !cmd) return;
    if (p->hiz.mip_count <= 1) return;
    if (!p->hiz.downsample_compute.valid()) return;

    gpu::bind_pipeline(cmd, p->hiz.downsample_compute);

    // One dispatch per mip transition (mip 0 → 1, 1 → 2, …). Each
    // dispatch downsamples a 2×2 source region per destination texel; the
    // 8×8 [numthreads] of hiz_build.slang means we issue ceil(dst_w/8) ×
    // ceil(dst_h/8) groups per mip.
    std::uint32_t src_w = p->desc.internal_width;
    std::uint32_t src_h = p->desc.internal_height;
    for (std::uint32_t dst_mip = 1; dst_mip < p->hiz.mip_count; ++dst_mip) {
        HizPushConstants pc{};
        pc.src_mip    = dst_mip - 1u;
        pc.dst_mip    = dst_mip;
        pc.src_width  = src_w;
        pc.src_height = src_h;
        gpu::push_constants(cmd, &pc, sizeof(pc), gpu::ShaderStage::Compute);

        const std::uint32_t dst_w = std::max(1u, src_w >> 1u);
        const std::uint32_t dst_h = std::max(1u, src_h >> 1u);
        const std::uint32_t gx = (dst_w + kHizTileSize - 1u) / kHizTileSize;
        const std::uint32_t gy = (dst_h + kHizTileSize - 1u) / kHizTileSize;
        gpu::dispatch(cmd, gx, gy, 1);
        ++p->stats.hiz_dispatches;

        src_w = dst_w;
        src_h = dst_h;
    }
}

} // namespace psynder::render::pipeline
