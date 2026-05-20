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
// Wave-C update — encode_light_cluster_build now issues a real
// bind_pipeline + dispatch via the psy::gpu encoder API. The shader binds
// (light_cluster_build.slang) come from lane 08's PSO registry.

#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <cstdio>
#include <cstdint>

namespace psynder::render::pipeline {

namespace {

constexpr std::uint32_t kMaxLightsPerScene     = 256;
constexpr std::uint32_t kAverageLightsPerCluster = 16;

// Workgroup size matching the shader [numthreads(64,1,1)] on
// light_cluster_build.slang::cs_build. Each group processes one cluster.
constexpr std::uint32_t kClusterGroupSize = 64;

// Push-constant block sent to the cluster-build compute. Mirrors the
// cbuffer in shaders/light_cluster_build.slang (PerView) — view + proj
// matrices live in the per-frame uniform path, but the cluster grid
// dimensions + light count are small enough to send inline.
struct ClusterPushConstants {
    std::uint32_t cluster_x;
    std::uint32_t cluster_y;
    std::uint32_t cluster_z;
    std::uint32_t light_count;
};

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
                                gpu::CmdBuffer* cmd,
                                const View& /*view*/) {
    if (!p || p->light_cluster.cluster_count == 0) return;
    if (!cmd) return;
    if (!p->light_cluster.build_compute.valid()) {
        // Compile failed at startup — nothing to dispatch. We already
        // logged once in init_light_cluster, so stay quiet here.
        return;
    }

    // Bind the cluster-build compute PSO. If lane 08 hasn't registered a
    // Metal MTLComputePipelineState for this id yet, MetalBackend silently
    // drops the dispatch — that's the documented "no PSO bound" path
    // (closes the encoder cleanly without recording any work). The pipeline
    // logically still walked the pass.
    gpu::bind_pipeline(cmd, p->light_cluster.build_compute);

    // Push the per-frame cluster constants. Light count is zero at scaffold
    // (no ECS lights yet — Issue lane09-003) so the shader writes zeros
    // into the cluster_light buffer header.
    ClusterPushConstants pc{};
    pc.cluster_x   = p->desc.light_clusters_x;
    pc.cluster_y   = p->desc.light_clusters_y;
    pc.cluster_z   = p->desc.light_clusters_z;
    pc.light_count = 0;
    gpu::push_constants(cmd, &pc, sizeof(pc), gpu::ShaderStage::Compute);

    // One workgroup per cluster (the shader's gid.x indexes the cluster).
    // The 64-thread group size lets each thread test a 64-stride chunk of
    // the scene light table; threadgroup atomics aggregate the result into
    // the per-cluster (offset, count) header.
    const std::uint32_t groups_x = p->light_cluster.cluster_count;
    (void)kClusterGroupSize; // documented contract with the shader [numthreads]
    gpu::dispatch(cmd, groups_x, 1, 1);
    ++p->stats.cluster_dispatches;
}

} // namespace psynder::render::pipeline
