// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/GpuCull.cpp
//
// Lane 09 — GPU-driven culling (compute → indirect draw arguments).
//
// DESIGN-PSYNDER-GX.md §7.1 step 4 + §7.2:
//   - One compute pass per view (main camera; shadow cascades later).
//   - Reads:  instance descriptor buffer (per-instance world AABB +
//             material/mesh ids), HiZ pyramid, view frustum constants.
//   - Writes: compact visible-instance index buffer + indirect-draw
//             argument buffer (vkCmdDrawIndexedIndirectCount-equivalent
//             / MTLIndirectCommandBuffer).
//
// Wave-C update — encode_gpu_cull now issues a real bind_pipeline +
// push_constants + dispatch. Group size 64 matches gpu_cull.slang's
// [numthreads(64,1,1)].

#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <cstdint>
#include <cstdio>

namespace psynder::render::pipeline {

namespace {

// Cap for the initial scaffold. Real scenes will rebuild on capacity
// growth (M3-M4 worlds); the size is per Pipeline, not per frame.
constexpr std::uint32_t kDefaultMaxInstances = 64 * 1024;

// Matches the [numthreads(64,1,1)] on gpu_cull.slang::cs_cull.
constexpr std::uint32_t kCullGroupSize = 64;

// Push-constant block sent to the cull compute. Mirrors the subset of
// gpu_cull.slang::CullParams that varies per frame; the static view-proj +
// frustum-planes uniform buffer is bound separately when lane 06 + lane
// 07's descriptor-set API land (Issue lane09-001 follow-up).
struct CullPushConstants {
    std::uint32_t instance_count;
    std::uint32_t hiz_mip_count;
    std::uint32_t padding[2];
};

constexpr std::size_t instance_desc_buffer_bytes() {
    // 128 bytes per instance × kDefaultMaxInstances. Matches the
    // InstanceDesc layout in gpu_cull.slang.
    return static_cast<std::size_t>(kDefaultMaxInstances) * 128u;
}

constexpr std::size_t visible_instance_buffer_bytes() {
    // One uint32 per instance + 16-byte count header.
    return 16u + static_cast<std::size_t>(kDefaultMaxInstances) * sizeof(std::uint32_t);
}

constexpr std::size_t indirect_draw_buffer_bytes() {
    // VkDrawIndexedIndirectCommand is 20 bytes; Metal equivalent ~ same.
    // Round up to 32 per slot for alignment. One slot per instance worst case.
    return static_cast<std::size_t>(kDefaultMaxInstances) * 32u;
}

} // namespace

bool init_gpu_cull(Pipeline* p) {
    if (!p) return false;
    p->gpu_cull.max_instances = kDefaultMaxInstances;

    if (p->device) {
        gpu::BufferDesc inst_desc{};
        inst_desc.size_bytes = instance_desc_buffer_bytes();
        inst_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Storage);
        inst_desc.heap       = gpu::HeapKind::DeviceLocal;
        inst_desc.debug_name = "instance_desc";
        p->gpu_cull.instance_desc_buffer = gpu::create_buffer(p->device, inst_desc);

        gpu::BufferDesc vis_desc{};
        vis_desc.size_bytes = visible_instance_buffer_bytes();
        vis_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Storage);
        vis_desc.heap       = gpu::HeapKind::DeviceLocal;
        vis_desc.debug_name = "visible_instance";
        p->gpu_cull.visible_instance_buffer = gpu::create_buffer(p->device, vis_desc);

        gpu::BufferDesc indir_desc{};
        indir_desc.size_bytes = indirect_draw_buffer_bytes();
        indir_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Indirect)
                              | static_cast<std::uint32_t>(gpu::BufferUsage::Storage);
        indir_desc.heap       = gpu::HeapKind::DeviceLocal;
        indir_desc.debug_name = "indirect_draw";
        p->gpu_cull.indirect_draw_buffer = gpu::create_buffer(p->device, indir_desc);

        if (!p->gpu_cull.instance_desc_buffer ||
            !p->gpu_cull.visible_instance_buffer ||
            !p->gpu_cull.indirect_draw_buffer)
        {
            std::fputs("[psy::render::pipeline] init_gpu_cull: buffer alloc failed\n", stderr);
            return false;
        }
    }

    shader::ComputePipelineDesc cdesc{};
    cdesc.slang_path     = shader_path("gpu_cull.slang");
    cdesc.entry_point_cs = "cs_cull";
    p->gpu_cull.cull_compute = shader::create_compute(cdesc);
    if (!p->gpu_cull.cull_compute.valid()) {
        std::fputs("[psy::render::pipeline] init_gpu_cull: compile failed; pass will be skipped\n",
                   stderr);
    }

    std::printf("[psy::render::pipeline] gpu_cull: max_instances=%u  inst_buf=%zu bytes  "
                "vis_buf=%zu bytes  indir_buf=%zu bytes  pipeline=%s\n",
                p->gpu_cull.max_instances,
                instance_desc_buffer_bytes(), visible_instance_buffer_bytes(),
                indirect_draw_buffer_bytes(),
                p->gpu_cull.cull_compute.valid() ? "ok" : "missing");
    return true;
}

void encode_gpu_cull(Pipeline* p,
                     gpu::CmdBuffer* cmd,
                     const View& /*view*/) {
    if (!p || !cmd) return;
    if (p->renderables.empty()) return;
    if (!p->gpu_cull.cull_compute.valid()) return;

    // Bind the cull compute PSO. (The visible_instance_buffer header and
    // the indirect_draw_buffer are bound via descriptor sets — currently
    // managed by lane 08's PipelineRegistry; the binding state survives
    // across multiple frames so we don't rebind per frame in this lane.)
    gpu::bind_pipeline(cmd, p->gpu_cull.cull_compute);

    CullPushConstants pc{};
    pc.instance_count = static_cast<std::uint32_t>(p->renderables.size());
    pc.hiz_mip_count  = p->hiz.mip_count;
    pc.padding[0]     = 0;
    pc.padding[1]     = 0;
    gpu::push_constants(cmd, &pc, sizeof(pc), gpu::ShaderStage::Compute);

    // Ceil(renderable_count / 64). The shader skips out-of-range threads.
    const std::uint32_t groups_x =
        (pc.instance_count + kCullGroupSize - 1u) / kCullGroupSize;
    gpu::dispatch(cmd, groups_x, 1, 1);
    ++p->stats.cull_dispatches;
}

} // namespace psynder::render::pipeline
