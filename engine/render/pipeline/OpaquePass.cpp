// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/OpaquePass.cpp
//
// Lane 09 — opaque forward+ pass.
//
// DESIGN-PSYNDER-GX.md §7.1 step 7 + §7.3:
//   - Single render pass writes colour to the swapchain (or HDR target +
//     post lane for M2) using the depth_texture from the depth pre-pass
//     as the depth attachment with depth-equal compare (we already laid
//     down depth in step 3).
//   - Per-fragment: derive cluster id from gl_FragCoord + projection,
//     iterate cluster's light list (cluster_light_buffer), accumulate
//     PBR metallic-roughness contribution.
//   - Draws via the cull pass's indirect_draw_buffer; CPU never iterates
//     the renderable list at submit time.
//
// Wave-C update — encode_opaque now issues a real begin_render +
// bind_pipeline + bind_vertex_buffer + bind_index_buffer + push_constants
// + draw_indexed sequence over the visible-instance set. The M1 path uses
// the m1_passthrough PSO (passthrough.slang from lane 08); the M2 path
// uses the m2_forward PSO from this lane's shaders/forward.slang. We
// currently prefer m2_forward when valid because forward.slang ships
// in-lane and is the M2 target.
//
// Indirect-draw with vkCmdDrawIndexedIndirectCount / MTLICB lands when
// lane 07 ships the indirect-draw cmd surface (Issue lane09-001 follow-up).
// Until then we walk renderables on the CPU and emit one draw_indexed per
// visible instance — equivalent work, just no GPU-side count consumption.

#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <cstdint>
#include <cstdio>

namespace psynder::render::pipeline {

namespace {

// Push-constant block for the opaque pass. Mirrors a subset of
// forward.slang's PerView + PerDraw cbuffers (the values that change per
// frame / per draw). The full uniform path comes via descriptor sets when
// lane 07 extends PublicGpu.h with cmd_bind_uniform_buffer — Issue
// lane09-001 follow-up. Total: 128 bytes (kMaxPushConstantBytes).
struct OpaquePushConstants {
    math::Mat4 view_proj;     // 64 bytes
    math::Mat4 world_matrix;  // 64 bytes
};
static_assert(sizeof(OpaquePushConstants) == 128,
              "OpaquePushConstants must equal kMaxPushConstantBytes");

// Build the GraphicsPipelineDesc for a colour + depth forward+ draw.
// Targets the swapchain colour format (Bgra8Srgb per Metal default) plus
// the Depth32Float from the HiZ depth_texture.
shader::GraphicsPipelineDesc build_forward_desc(const char* slang_path,
                                                const char* entry_vs,
                                                const char* entry_fs) {
    shader::GraphicsPipelineDesc desc{};
    desc.slang_path        = slang_path;
    desc.entry_point_vs    = entry_vs;
    desc.entry_point_fs    = entry_fs;
    desc.color_format_count = 1;
    desc.color_formats[0]   = static_cast<std::uint8_t>(gpu::Format::Bgra8Srgb);
    desc.depth_format       = static_cast<std::uint8_t>(gpu::Format::Depth32Float);
    desc.enable_depth_write = false;  // pre-pass already wrote; depth-equal here
    desc.enable_blend       = false;
    return desc;
}

math::Mat4 compute_view_proj(const View& view) {
    return math::mul(view.proj_matrix, view.view_matrix);
}

} // namespace

bool init_opaque_pass(Pipeline* p) {
    if (!p) return false;

    // M1 — passthrough.slang. Lane 08 shipped this; we just compile to
    // a pipeline. The slang_path here is a literal because lane 08 owns
    // it; we don't route through our shader_path() helper.
    auto m1_desc = build_forward_desc(
        "engine/shader/builtin/passthrough.slang", "vs_main", "fs_main");
    m1_desc.enable_depth_write = true; // M1 has no separate depth pre-pass
    p->opaque_pass.m1_passthrough = shader::create_graphics(m1_desc);
    if (!p->opaque_pass.m1_passthrough.valid()) {
        std::fputs("[psy::render::pipeline] init_opaque_pass: M1 passthrough compile failed\n",
                   stderr);
    }

    // M2 — forward.slang (PBR metallic-roughness). Shipped by this lane.
    auto m2_desc = build_forward_desc(
        shader_path("forward.slang"), "vs_main", "fs_main");
    p->opaque_pass.m2_forward = shader::create_graphics(m2_desc);
    if (!p->opaque_pass.m2_forward.valid()) {
        std::fputs("[psy::render::pipeline] init_opaque_pass: M2 forward compile failed\n",
                   stderr);
    }

    std::printf("[psy::render::pipeline] opaque_pass: m1_passthrough=%s  m2_forward=%s\n",
                p->opaque_pass.m1_passthrough.valid() ? "ok" : "missing",
                p->opaque_pass.m2_forward.valid()     ? "ok" : "missing");
    return p->opaque_pass.m1_passthrough.valid() || p->opaque_pass.m2_forward.valid();
}

void encode_opaque(Pipeline* p,
                   gpu::CmdBuffer* cmd,
                   const View& view) {
    if (!p || !cmd) return;
    if (p->renderables.empty()) return;

    // Choose the PSO. Prefer the M2 PBR forward shader (this lane's
    // shaders/forward.slang) when it compiled; fall back to the M1
    // passthrough so the M1 triangle path still renders even if a M2
    // shader regression breaks forward.slang.
    shader::PipelineHandle pso{};
    bool depth_write = false;
    if (p->opaque_pass.m2_forward.valid()) {
        pso = p->opaque_pass.m2_forward;
        depth_write = false; // depth pre-pass already laid down depth
    } else if (p->opaque_pass.m1_passthrough.valid()) {
        pso = p->opaque_pass.m1_passthrough;
        depth_write = true;  // M1 path has no separate depth pre-pass
    } else {
        return; // no usable PSO
    }
    (void)depth_write; // depth-write flag is baked into the PSO at compile

    // Color = swapchain image with clear; depth = pre-pass target (loaded
    // so we can do depth-equal compare in the fragment shader). In M1 the
    // pre-pass didn't run so we clear here too — controlled via the chosen
    // PSO above (m1_passthrough has depth_write=true).
    gpu::RenderPassDesc rpd{};
    rpd.swapchain       = true;
    rpd.color_count     = 1;
    rpd.colors[0].target = nullptr; // swapchain image
    rpd.colors[0].load   = gpu::LoadOp::Clear;
    rpd.colors[0].store  = gpu::StoreOp::Store;
    rpd.colors[0].clear_rgba[0] = 0.02f;
    rpd.colors[0].clear_rgba[1] = 0.02f;
    rpd.colors[0].clear_rgba[2] = 0.03f;
    rpd.colors[0].clear_rgba[3] = 1.0f;
    if (p->hiz.depth_texture && p->opaque_pass.m2_forward.valid()) {
        rpd.depth.target = p->hiz.depth_texture.get();
        rpd.depth.load   = gpu::LoadOp::Load;   // pre-pass result is live
        rpd.depth.store  = gpu::StoreOp::DontCare;
    } else {
        rpd.depth.target = nullptr;
    }

    gpu::begin_render(cmd, rpd);
    ++p->stats.opaque_passes;

    // Viewport / scissor from the View. For M1 the View viewport may be
    // zero — fall back to the pipeline's internal target size.
    const std::uint32_t vw = view.viewport_w ? view.viewport_w : p->desc.internal_width;
    const std::uint32_t vh = view.viewport_h ? view.viewport_h : p->desc.internal_height;
    gpu::Viewport vp{
        static_cast<float>(view.viewport_x),
        static_cast<float>(view.viewport_y),
        static_cast<float>(vw),
        static_cast<float>(vh),
        0.0f, 1.0f
    };
    gpu::set_viewport(cmd, vp);
    gpu::Scissor sc{
        static_cast<std::int32_t>(view.viewport_x),
        static_cast<std::int32_t>(view.viewport_y),
        vw, vh
    };
    gpu::set_scissor(cmd, sc);

    gpu::bind_pipeline(cmd, pso);

    // DOTS: iterate the visible-instance flat array. No virtual / vtable.
    // The CPU walk here is the placeholder for the GPU indirect-draw path
    // (Issue lane09-001 follow-up) — one draw_indexed per visible instance.
    const math::Mat4 view_proj = compute_view_proj(view);
    for (const Renderable& r : p->renderables) {
        if (!r.vertex_buffer || !r.index_buffer) continue;

        gpu::bind_vertex_buffer(cmd, 0, r.vertex_buffer);
        gpu::bind_index_buffer (cmd, r.index_buffer, gpu::IndexType::U16);

        OpaquePushConstants pc{};
        pc.view_proj    = view_proj;
        pc.world_matrix = r.world_matrix;
        gpu::push_constants(cmd, &pc, sizeof(pc), gpu::ShaderStage::AllGfx);

        gpu::draw_indexed(cmd, r.index_count);
        ++p->stats.opaque_draws;
    }

    gpu::end_render(cmd);
}

} // namespace psynder::render::pipeline
