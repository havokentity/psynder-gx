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
// Wave-B scope:
//   - Allocate three persistent buffers sized for kDefaultMaxInstances.
//   - Create the cull compute pipeline via lane 08.
//   - encode_gpu_cull: log + drop until lane 07 ships cmd encoder APIs.

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

// Per-instance descriptor for GPU cull:
//   float4x4 world_matrix       (64 bytes)
//   float4   aabb_min_pad       (16 bytes)
//   float4   aabb_max_pad       (16 bytes)
//   uint32   mesh_id            (4)
//   uint32   material_id        (4)
//   uint32   pad[2]             (8)
// Total: 112 bytes — round up to 128 for natural alignment.
constexpr std::size_t kInstanceDescBytes = 128;

constexpr std::size_t instance_desc_buffer_bytes() {
    return static_cast<std::size_t>(kDefaultMaxInstances) * kInstanceDescBytes;
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
                     gpu::CmdBuffer* /*cmd*/,
                     const View& /*view*/) {
    if (!p) return;
    // Real encode: bind instance_desc + visible_instance + indirect_draw +
    // HiZ + view frustum UBO; dispatch ceil(renderable_count / 64). The
    // compute shader (gpu_cull.slang) does frustum + HiZ tests per
    // instance and atomically appends to visible_instance + builds the
    // indirect-draw record. Blocked on lane 07 cmd encoder APIs.
    if (!p->warned_missing_cmd_encoder && !p->renderables.empty()) {
        std::printf("[psy::render::pipeline] gpu_cull would dispatch %zu instances "
                    "(%zu groups of 64)\n",
                    p->renderables.size(),
                    (p->renderables.size() + 63u) / 64u);
    }
}

} // namespace psynder::render::pipeline
