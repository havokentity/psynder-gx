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
// Wave-B scope:
//   - Create two graphics pipelines via lane 08:
//       m1_passthrough  — engine/shader/builtin/passthrough.slang
//                         (used by the M1 textured-triangle path)
//       m2_forward      — engine/render/pipeline/shaders/forward.slang
//                         (M2 PBR metallic-roughness)
//   - encode_opaque: at scaffold time we just log the would-be draw call
//     shape. The real cmd_begin_render / cmd_draw_indexed_indirect plumb
//     in once lane 07 ships the cmd encoder APIs (Issue lane09-001).

#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <cstdint>
#include <cstdio>

namespace psynder::render::pipeline {

namespace {

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
                   gpu::CmdBuffer* /*cmd*/,
                   const View& /*view*/) {
    if (!p || p->renderables.empty()) return;

    // Real encode (M2 ship target):
    //   cmd_begin_render(color = swapchain, depth = hiz.depth_texture)
    //   cmd_bind_pipeline(opaque_pass.m2_forward)   // (M1: m1_passthrough)
    //   cmd_bind_storage_buffer(0, gpu_cull.visible_instance_buffer)
    //   cmd_bind_storage_buffer(1, light_cluster.cluster_light_buffer)
    //   cmd_bind_storage_buffer(2, gpu_cull.instance_desc_buffer)
    //   cmd_set_viewport(view.viewport_*)
    //   cmd_draw_indexed_indirect_count(gpu_cull.indirect_draw_buffer,
    //                                   visible_count_offset)
    //   cmd_end_render()
    //
    // Blocked on lane 07 cmd encoder APIs. Until then this lane logs the
    // first frame's intent and emits a one-time warning so the integration
    // engineer can see we are wired in correctly.
    if (!p->warned_missing_cmd_encoder) {
        std::printf("[psy::render::pipeline] opaque would draw %zu renderables "
                    "(pipeline=%s, frame=%llu)\n",
                    p->renderables.size(),
                    p->opaque_pass.m1_passthrough.valid() ? "m1_passthrough"
                  : p->opaque_pass.m2_forward.valid()     ? "m2_forward"
                  : "(none)",
                    static_cast<unsigned long long>(p->frame_index));
        std::fputs("[psy::render::pipeline] NOTE: cmd encoder APIs not in PublicGpu.h; "
                   "falling back to lane-07 backend's M0 animated clear. "
                   "See INTEGRATION.txt + Issue lane09-001.\n",
                   stderr);
        p->warned_missing_cmd_encoder = true;
    }
}

} // namespace psynder::render::pipeline
