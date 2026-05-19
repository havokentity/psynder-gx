// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/Resources.cpp
//
// Lane 09 — M1 triangle vertex / index buffer construction + central
// resource teardown.
//
// The M1 acceptance criterion is "rotating textured triangle". The
// triangle here is screen-space (clip-space directly) so it renders
// without a view/proj transform, which keeps the M1 demo decoupled from
// camera matrix correctness. Lane 09 owns the data; lane 07's GPU
// abstraction owns the API allocation; lane 08's shader cache owns the
// pipeline compile.

#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace psynder::render::pipeline {

namespace {

// Three NDC-space vertices, RGB per-vertex tint. Front face winding is CCW
// (matches the default in both Vulkan and Metal).
constexpr std::array<Vertex, 3> kTriangleVertices = {{
    // position                normal           uv             color (RGBA)
    {{ 0.0f,  0.6f, 0.0f}, {0,0,1}, {0.5f, 0.0f}, {1.0f, 0.2f, 0.2f, 1.0f}},
    {{-0.6f, -0.4f, 0.0f}, {0,0,1}, {0.0f, 1.0f}, {0.2f, 1.0f, 0.2f, 1.0f}},
    {{ 0.6f, -0.4f, 0.0f}, {0,0,1}, {1.0f, 1.0f}, {0.2f, 0.2f, 1.0f, 1.0f}},
}};

constexpr std::array<std::uint16_t, 3> kTriangleIndices = {0, 1, 2};

} // namespace

bool upload_m1_triangle(Pipeline* p) {
    if (!p || !p->device) {
        // Headless mode — leave triangle resources empty; render() will
        // skip the M1 path because vertex_buffer.get() == nullptr.
        return false;
    }

    // ─── Vertex buffer ──────────────────────────────────────────────────
    gpu::BufferDesc vb_desc{};
    vb_desc.size_bytes = sizeof(kTriangleVertices);
    vb_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Vertex);
    // Apple Silicon: unified memory, prefer DeviceLocal direct-map.
    // Win/Linux: still pick DeviceLocal here; lane 07's Vulkan backend
    // will route through a staging copy internally. M2 may switch to an
    // explicit HostVisible staging path if profiling shows overhead.
    vb_desc.heap       = gpu::device_is_unified_memory(p->device)
                       ? gpu::HeapKind::DeviceLocal
                       : gpu::HeapKind::HostVisible;
    vb_desc.debug_name = "m1_triangle_vb";

    p->m1_triangle.vertex_buffer = gpu::create_buffer(p->device, vb_desc);
    if (!p->m1_triangle.vertex_buffer) {
        std::fputs("[psy::render::pipeline] upload_m1_triangle: VB alloc failed\n", stderr);
        return false;
    }
    if (void* mapped = gpu::buffer_map(p->m1_triangle.vertex_buffer.get())) {
        std::memcpy(mapped, kTriangleVertices.data(), sizeof(kTriangleVertices));
        gpu::buffer_unmap(p->m1_triangle.vertex_buffer.get());
    } else {
        // Lane 07's M0 backend returns nullptr from buffer_map for
        // stubbed buffers — we accept that and treat the M1 path as
        // "resources allocated but content not yet uploaded". Once lane
        // 07 finishes the M1 mapping path the memcpy above will succeed
        // and the triangle will render. This is logged once at startup.
        std::fputs("[psy::render::pipeline] upload_m1_triangle: buffer_map returned null "
                   "(lane 07 M0 stub) — VB content deferred\n", stderr);
    }
    p->m1_triangle.vertex_count = static_cast<std::uint32_t>(kTriangleVertices.size());

    // ─── Index buffer ───────────────────────────────────────────────────
    gpu::BufferDesc ib_desc{};
    ib_desc.size_bytes = sizeof(kTriangleIndices);
    ib_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Index);
    ib_desc.heap       = gpu::device_is_unified_memory(p->device)
                       ? gpu::HeapKind::DeviceLocal
                       : gpu::HeapKind::HostVisible;
    ib_desc.debug_name = "m1_triangle_ib";

    p->m1_triangle.index_buffer = gpu::create_buffer(p->device, ib_desc);
    if (!p->m1_triangle.index_buffer) {
        std::fputs("[psy::render::pipeline] upload_m1_triangle: IB alloc failed\n", stderr);
        return false;
    }
    if (void* mapped = gpu::buffer_map(p->m1_triangle.index_buffer.get())) {
        std::memcpy(mapped, kTriangleIndices.data(), sizeof(kTriangleIndices));
        gpu::buffer_unmap(p->m1_triangle.index_buffer.get());
    }
    p->m1_triangle.index_count = static_cast<std::uint32_t>(kTriangleIndices.size());

    std::printf("[psy::render::pipeline] M1 triangle uploaded: %u verts / %u indices\n",
                p->m1_triangle.vertex_count, p->m1_triangle.index_count);
    return true;
}

void release_pipeline_resources(Pipeline* p) {
    if (!p) return;
    // gpu::Handle<T> destructors handle the deferred destroy + refcount
    // drop via lane 07's backend->destroy_resource. We just clear them
    // explicitly so debug builds can confirm via Handle::reset semantics
    // (assignment to default-constructed Handle releases).
    p->m1_triangle.vertex_buffer    = gpu::Handle<gpu::Buffer>{};
    p->m1_triangle.index_buffer     = gpu::Handle<gpu::Buffer>{};
    p->m1_triangle.vertex_count     = 0;
    p->m1_triangle.index_count      = 0;

    p->light_cluster.cluster_aabb_buffer  = gpu::Handle<gpu::Buffer>{};
    p->light_cluster.cluster_light_buffer = gpu::Handle<gpu::Buffer>{};
    p->light_cluster.cluster_count        = 0;

    p->hiz.depth_texture = gpu::Handle<gpu::Texture>{};
    p->hiz.hiz_pyramid   = gpu::Handle<gpu::Texture>{};
    p->hiz.mip_count     = 0;

    p->gpu_cull.instance_desc_buffer    = gpu::Handle<gpu::Buffer>{};
    p->gpu_cull.visible_instance_buffer = gpu::Handle<gpu::Buffer>{};
    p->gpu_cull.indirect_draw_buffer    = gpu::Handle<gpu::Buffer>{};
    p->gpu_cull.max_instances           = 0;

    // Shader pipelines are owned by lane 08's PipelineRegistry; we tell
    // it to drop our handles so the cache can evict them.
    if (p->light_cluster.build_compute.valid()) {
        shader::destroy_pipeline(p->light_cluster.build_compute);
        p->light_cluster.build_compute = {};
    }
    if (p->hiz.downsample_compute.valid()) {
        shader::destroy_pipeline(p->hiz.downsample_compute);
        p->hiz.downsample_compute = {};
    }
    if (p->gpu_cull.cull_compute.valid()) {
        shader::destroy_pipeline(p->gpu_cull.cull_compute);
        p->gpu_cull.cull_compute = {};
    }
    if (p->opaque_pass.m1_passthrough.valid()) {
        shader::destroy_pipeline(p->opaque_pass.m1_passthrough);
        p->opaque_pass.m1_passthrough = {};
    }
    if (p->opaque_pass.m2_forward.valid()) {
        shader::destroy_pipeline(p->opaque_pass.m2_forward);
        p->opaque_pass.m2_forward = {};
    }

    p->renderables.clear();
}

// ─── Slang path resolver ────────────────────────────────────────────────
//
// The render-pipeline lane ships its own .slang sources under
// engine/render/pipeline/shaders/. Lane 08's compile path takes an
// absolute filesystem path (or a path relative to the working dir of
// the running binary). We assume the binary runs from the repo root for
// development; production VFS lookup is M2+. See INTEGRATION.txt.
const char* shader_path(const char* leaf) {
    static thread_local char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "engine/render/pipeline/shaders/%s", leaf);
    return buf;
}

} // namespace psynder::render::pipeline
