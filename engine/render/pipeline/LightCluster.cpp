// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/LightCluster.cpp
//
// Lane 09 — forward+ light clustering, build pass (compute).
//
// DESIGN-PSYNDER-GX.md §7.3:
//   - Subdivide view frustum into a 3D grid (default 16 × 9 × 24).
//   - Compute pass per frame: per-cluster light list (light indices that
//     intersect the cluster AABB in view space).
//   - Fragment shader reads (cluster id → light list) at sampling time.
//
// Storage:
//   cluster_aabb_buffer  — 32 bytes per cluster (min_xyz/pad + max_xyz/pad).
//                          Rebuilt only when projection changes (rare).
//   cluster_light_buffer — variable layout: per-cluster (offset, count) header
//                          + flat light-index array. Sized for up to 256
//                          lights × ~16 affecting clusters average (the
//                          DESIGN target).
//
// Wave-B scope:
//   - Allocate both buffers via psy::gpu::create_buffer.
//   - Create the build compute pipeline via psy::shader::create_compute
//     from shaders/light_cluster_build.slang.
//   - encode_light_cluster_build is a stub — see Pipeline.cpp + INTEGRATION
//     for why (no cmd encoder API on PublicGpu.h yet). The resources +
//     pipeline are ready so the encode lights up once lane 07 expands the
//     contract.

#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <cstdio>
#include <cstdint>

namespace psynder::render::pipeline {

namespace {

constexpr std::uint32_t kMaxLightsPerScene     = 256;
constexpr std::uint32_t kAverageLightsPerCluster = 16;

constexpr std::size_t cluster_aabb_bytes(std::uint32_t cluster_count) {
    // Two Vec4 per cluster (min.xyz/pad, max.xyz/pad).
    return static_cast<std::size_t>(cluster_count) * (2u * 4u * sizeof(float));
}

constexpr std::size_t cluster_light_bytes(std::uint32_t cluster_count) {
    // Per-cluster header: (uint32 offset, uint32 count) = 8 bytes.
    // Light-index array: averageLightsPerCluster * sizeof(uint32_t) per cluster.
    const std::size_t headers = static_cast<std::size_t>(cluster_count) * 2u * sizeof(std::uint32_t);
    const std::size_t indices = static_cast<std::size_t>(cluster_count)
                              * kAverageLightsPerCluster * sizeof(std::uint32_t);
    return headers + indices;
}

} // namespace

bool init_light_cluster(Pipeline* p) {
    if (!p) return false;
    LightCluster& lc = p->light_cluster;
    lc.cluster_count = p->desc.light_clusters_x
                     * p->desc.light_clusters_y
                     * p->desc.light_clusters_z;

    if (lc.cluster_count == 0) {
        std::fputs("[psy::render::pipeline] init_light_cluster: zero clusters; skipping\n", stderr);
        return false;
    }

    if (p->device) {
        gpu::BufferDesc aabb_desc{};
        aabb_desc.size_bytes = cluster_aabb_bytes(lc.cluster_count);
        aabb_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Storage);
        aabb_desc.heap       = gpu::HeapKind::DeviceLocal;
        aabb_desc.debug_name = "cluster_aabb";
        lc.cluster_aabb_buffer = gpu::create_buffer(p->device, aabb_desc);

        gpu::BufferDesc light_desc{};
        light_desc.size_bytes = cluster_light_bytes(lc.cluster_count);
        light_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Storage);
        light_desc.heap       = gpu::HeapKind::DeviceLocal;
        light_desc.debug_name = "cluster_light";
        lc.cluster_light_buffer = gpu::create_buffer(p->device, light_desc);

        if (!lc.cluster_aabb_buffer || !lc.cluster_light_buffer) {
            std::fputs("[psy::render::pipeline] init_light_cluster: buffer alloc failed\n", stderr);
            return false;
        }
    }

    // Compile the cluster-build compute via lane 08. The slang source lives
    // in this lane (engine/render/pipeline/shaders/light_cluster_build.slang).
    shader::ComputePipelineDesc cdesc{};
    cdesc.slang_path     = shader_path("light_cluster_build.slang");
    cdesc.entry_point_cs = "cs_build";
    lc.build_compute = shader::create_compute(cdesc);
    if (!lc.build_compute.valid()) {
        // Compile failure is non-fatal at scaffold time — the encode step
        // will skip the dispatch and log. Lane 08's create_compute writes
        // the slang diagnostic to stderr already.
        std::fputs("[psy::render::pipeline] init_light_cluster: compile failed; "
                   "pass will be skipped\n", stderr);
    }

    std::printf("[psy::render::pipeline] light_cluster: %ux%ux%u = %u clusters  "
                "aabb=%zu bytes  lights=%zu bytes  max_lights=%u  pipeline=%s\n",
                p->desc.light_clusters_x, p->desc.light_clusters_y, p->desc.light_clusters_z,
                lc.cluster_count, cluster_aabb_bytes(lc.cluster_count),
                cluster_light_bytes(lc.cluster_count),
                kMaxLightsPerScene,
                lc.build_compute.valid() ? "ok" : "missing");
    return true;
}

void encode_light_cluster_build(Pipeline* p,
                                gpu::CmdBuffer* /*cmd*/,
                                const View& /*view*/) {
    if (!p || p->light_cluster.cluster_count == 0) return;

    // The actual encode (cmd_bind_pipeline(cull) + bind cluster_aabb_buffer
    // + cluster_light_buffer + light_table_buffer + cmd_dispatch(x/y/z)) is
    // blocked on lane 07 exposing the cmd encoder APIs. Until then we log
    // the would-be dispatch shape once. See INTEGRATION + Issue lane09-001.
    if (!p->warned_missing_cmd_encoder) {
        std::printf("[psy::render::pipeline] light_cluster_build would dispatch "
                    "%u groups x 1 x 1 (one group per cluster, 32-light test per group)\n",
                    p->light_cluster.cluster_count / 32u
                      + (p->light_cluster.cluster_count % 32u ? 1u : 0u));
    }
}

} // namespace psynder::render::pipeline
