// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/Pipeline_internal.h
//
// Lane 09 — INTERNAL implementation header. Only included by .cpp files in
// engine/render/pipeline/. Other lanes must not include this; they only
// see PublicRenderPipeline.h.
//
// This header gives the opaque psy::render::pipeline::{Pipeline, View}
// types from PublicRenderPipeline.h concrete bodies that carry the GPU
// resources, the per-frame extracted draw stream, and the scaffolded
// forward+ sub-pass state (light cluster, HiZ pyramid, GPU cull buffers,
// opaque pass pipeline objects).
//
// See DESIGN-PSYNDER-GX.md §7.1–§7.3 for the per-frame flow this header
// is decomposing.

#pragma once

#include "render/pipeline/PublicRenderPipeline.h"

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"
#include "math/Math.h"

#include <cstdint>
#include <vector>
#include <string>

namespace psynder::render::pipeline {

// ─── Vertex format for the M1 triangle / M2 PBR sphere path ─────────────
//
// Position(3) + Normal(3) + UV(2) + Color(4) — 12 floats = 48 bytes.
// This format also matches engine/shader/builtin/passthrough.slang for the
// M1 textured-triangle demo path, plus shaders/forward.slang for the M2
// PBR-on-glTF path (normal/uv used, color is per-instance tint).
struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec4 color;
};

// ─── Per-draw "renderable" produced by the ECS extract step ─────────────
//
// In M1 we extract a single hardcoded triangle (no real ECS scan); the
// extract function returns a single Renderable entry whose vertex/index
// buffer come from the Pipeline's pre-allocated M1 resources. In M2+ the
// extract walks lane 06 scene queries for entities with a MeshRef +
// MaterialRef + Transform component and produces one Renderable per
// instance. The render() entry point then turns that list into GPU draw
// arguments (indirect, via the cull compute pass scaffold).
struct Renderable {
    gpu::Buffer* vertex_buffer = nullptr;  // owned by Pipeline (Handle in storage)
    gpu::Buffer* index_buffer  = nullptr;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count  = 0;
    math::Mat4    world_matrix{};          // identity for M1
    std::uint32_t material_id  = 0;        // M2+: index into material table
};

// ─── Forward+ light cluster grid (compute pass scaffold) ────────────────
//
// 3D grid of clusters in view-space, sized from PipelineDesc:
//   total_clusters = light_clusters_x * light_clusters_y * light_clusters_z
//
// Two GPU buffers back the system (DESIGN §7.3):
//   cluster_aabb_buffer : per-cluster view-space AABB (built once or on
//                         camera-fov change), used by the cluster-light
//                         intersection compute pass.
//   cluster_light_buffer: per-cluster light list (variable-length, offsets
//                         + counts encoded as two uint32_t per cluster).
//
// The compute pass that fills cluster_light_buffer is dispatched per
// frame as the "build" pass; the fragment shader then reads the same
// buffer through a structured-buffer binding.
struct LightCluster {
    std::uint32_t       cluster_count = 0;    // x * y * z
    gpu::Handle<gpu::Buffer> cluster_aabb_buffer;
    gpu::Handle<gpu::Buffer> cluster_light_buffer;
    shader::PipelineHandle   build_compute{};  // light cluster build cs
};

// ─── Depth pre-pass + HiZ pyramid (compute) ─────────────────────────────
//
// Depth pre-pass renders the opaque set into a depth texture (no color),
// then a chain of compute dispatches downsamples it into a min/max
// pyramid for the GPU cull pass to do conservative occlusion tests.
//
// At Wave-B scaffold time we hold the resources + pipeline objects; the
// actual encode happens once lane 07 adds cmd encoder APIs (see
// INTEGRATION.txt for the cross-lane gap and the issue we file).
struct HizPyramid {
    gpu::Handle<gpu::Texture> depth_texture;   // Format::Depth32Float
    gpu::Handle<gpu::Texture> hiz_pyramid;     // mipped, R32Uint min/max
    std::uint32_t             mip_count = 0;
    shader::PipelineHandle    downsample_compute{};
    // Depth-only graphics PSO for the pre-pass — VS-only (no color writes),
    // depth-write enabled. Sourced from shaders/depth_only.slang.
    shader::PipelineHandle    depth_only_graphics{};
};

// ─── GPU-driven cull (compute → indirect draw arguments) ────────────────
//
// One dispatch per view (main camera; later: shadow cascades). Reads the
// per-instance descriptor buffer + HiZ pyramid, writes:
//   visible_instance_buffer : compact list of visible instance indices
//   indirect_draw_buffer    : VkDrawIndexedIndirectCommand-equivalent
struct GpuCull {
    gpu::Handle<gpu::Buffer> instance_desc_buffer;
    gpu::Handle<gpu::Buffer> visible_instance_buffer;
    gpu::Handle<gpu::Buffer> indirect_draw_buffer;
    std::uint32_t            max_instances = 0;
    shader::PipelineHandle   cull_compute{};
};

// ─── Opaque forward+ pass pipeline objects ──────────────────────────────
//
// One graphics pipeline per (material × vertex layout). M1 uses a single
// hard-coded passthrough pipeline for the triangle. M2 uses
// shaders/forward.slang with PBR metallic-roughness.
struct OpaquePass {
    shader::PipelineHandle m1_passthrough{}; // M1: passthrough.slang
    shader::PipelineHandle m2_forward{};     // M2: forward.slang
};

// ─── Storage for the M1 hardcoded triangle ──────────────────────────────
//
// We pre-allocate one vertex + one index buffer for the M1 demo path so
// every render() call can re-issue the same draw without going through
// the ECS scan. M2+ replaces this with a real instance pool.
struct M1TriangleResources {
    gpu::Handle<gpu::Buffer> vertex_buffer;
    gpu::Handle<gpu::Buffer> index_buffer;
    std::uint32_t            vertex_count = 0;
    std::uint32_t            index_count  = 0;
};

// ─── View (per-frame camera + viewport + RT acceleration handle) ────────
//
// Public surface (PublicRenderPipeline.h) forward-declares this; we give
// it a body here so the call site can construct it. Callers populate
// view_matrix / proj_matrix; the pipeline derives the view frustum +
// per-view cull data internally.
struct View {
    math::Mat4    view_matrix{};
    math::Mat4    proj_matrix{};
    std::uint32_t viewport_x = 0;
    std::uint32_t viewport_y = 0;
    std::uint32_t viewport_w = 0;
    std::uint32_t viewport_h = 0;

    // M5 — RT acceleration structure pointer per view (shadow rays from
    // the camera origin). Left nullptr until lane 10 wires it in.
    gpu::AccelerationStructure* tlas = nullptr;
};

// ─── Pipeline — top-level holder for all the above ──────────────────────
//
// One Pipeline instance per app run. Owns the GPU device pointer (does
// NOT own its lifetime — the application created the device and passes
// it implicitly through PipelineDesc + handle indirection). Owns the
// extracted-draw list, cluster + HiZ + cull state, and the per-pass
// shader pipelines.
struct Pipeline {
    PipelineDesc desc{};

    // The application-owned device. Stashed at create() so the per-frame
    // render() path can call psy::gpu::cmd_open / cmd_submit. NOT owned.
    gpu::Device* device = nullptr;

    // Sub-pass state (DESIGN §7.1 step ordering)
    LightCluster        light_cluster{};
    HizPyramid          hiz{};
    GpuCull             gpu_cull{};
    OpaquePass          opaque_pass{};

    // M1 demo resources
    M1TriangleResources m1_triangle{};

    // Per-frame extracted draw list. extract() refills; render() consumes.
    std::vector<Renderable> renderables;

    // Frame counter for diagnostics + animation
    std::uint64_t frame_index = 0;

    // Diagnostics — set when scaffold passes fall through to the M0
    // backend-encoded clear path because cmd-encoding APIs aren't wired
    // through psy::gpu yet. See INTEGRATION.txt + Issue lane09-001.
    bool warned_missing_cmd_encoder = false;

    // ─── Per-frame draw / dispatch counters ─────────────────────────────
    //
    // Bumped by each encode_*() entry point as it issues real cmd encoder
    // calls. The test harness reads these to verify the pipeline actually
    // walked the expected sub-passes — they're cheap (one uint64 per pass)
    // and live regardless of whether a real device is attached.
    struct PassStats {
        std::uint64_t opaque_draws        = 0; // draw_indexed calls (opaque pass)
        std::uint64_t depth_prepass_draws = 0; // draw_indexed calls (depth pre-pass)
        std::uint64_t cluster_dispatches  = 0; // light_cluster_build dispatches
        std::uint64_t hiz_dispatches      = 0; // hiz_downsample dispatches
        std::uint64_t cull_dispatches     = 0; // gpu_cull dispatches
        std::uint64_t opaque_passes       = 0; // begin_render(opaque) count
        std::uint64_t depth_passes        = 0; // begin_render(depth) count
    };
    PassStats stats{};
};

// ─── Stats accessor for tests / diagnostics ─────────────────────────────
//
// Free function so tests don't have to take the dependency on the full
// Pipeline_internal.h layout — they just include this header at the call
// site (it's in the same lane subtree).
const Pipeline::PassStats& pipeline_stats(const Pipeline* p);

// ─── Internal helpers (defined in the various sub-pass .cpp files) ──────

// Resources.cpp
bool upload_m1_triangle(Pipeline* p);
void release_pipeline_resources(Pipeline* p);

// LightCluster.cpp
bool init_light_cluster(Pipeline* p);
void encode_light_cluster_build(Pipeline* p, gpu::CmdBuffer* cmd, const View& view);

// Hiz.cpp
bool init_hiz_pyramid(Pipeline* p);
void encode_depth_prepass(Pipeline* p, gpu::CmdBuffer* cmd, const View& view);
void encode_hiz_downsample(Pipeline* p, gpu::CmdBuffer* cmd);

// GpuCull.cpp
bool init_gpu_cull(Pipeline* p);
void encode_gpu_cull(Pipeline* p, gpu::CmdBuffer* cmd, const View& view);

// OpaquePass.cpp
bool init_opaque_pass(Pipeline* p);
void encode_opaque(Pipeline* p, gpu::CmdBuffer* cmd, const View& view);

// Extract.cpp
void extract_renderables(Pipeline* p, const View& view);

// Returns the absolute filesystem path of a Slang source under
// engine/render/pipeline/shaders/. The render-pipeline lane ships its
// own .slang sources here; lane 08's shader cache compiles them.
const char* shader_path(const char* leaf);

} // namespace psynder::render::pipeline
