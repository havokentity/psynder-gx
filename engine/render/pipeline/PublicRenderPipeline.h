// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/PublicRenderPipeline.h
//
// Lane 09 — Render pipeline public-header CONTRACT (Wave 0).
// Other lanes only `#include "engine/render/pipeline/PublicRenderPipeline.h"`.
// Lane agent: implement against this header; if you need to change the
// contract, file an Issue against the orchestrator.
//
// Owns the GPU-driven forward+ path: depth pre-pass + HiZ pyramid + GPU
// cull + light cluster build + opaque/transparent passes. RT and post
// integrate through separate Public*.h files (lanes 10 / 11).
//
// See DESIGN-PSYNDER-GX.md §7.1–§7.3 for the pipeline at a glance.

#pragma once

#include <cstdint>

namespace psynder::render::pipeline {

// Opaque handle to a constructed forward+ pipeline instance, holding all the
// internal GPU resources (descriptor sets, framebuffer attachments, light
// cluster buffer, HiZ pyramid, etc.). Lifetime: app-level.
struct Pipeline;

// Per-frame view: a camera + viewport + per-view RT acceleration structure.
// Multiple views per frame for shadow cascades, RT secondary cameras, etc.
struct View;

// Settings selected from the level / quality preset. Frozen for the level.
struct PipelineDesc {
    std::uint32_t internal_width   = 1920;
    std::uint32_t internal_height  = 1080;
    std::uint32_t shadow_cascades  = 4;    // CSM cascades when RT shadows are off
    std::uint32_t light_clusters_x = 16;
    std::uint32_t light_clusters_y = 9;
    std::uint32_t light_clusters_z = 24;
    bool          enable_rt        = true; // capability gate; lane 10 owns impl
    bool          enable_mesh_shaders = true;
};

// Create + destroy a pipeline. Hooks into lane 07 (gpu) for resources.
Pipeline* create(const PipelineDesc&);
void      destroy(Pipeline*);

// Per-frame entry: gather visible instances from the ECS (lane 06), build
// indirect draw arguments via compute cull pass, execute the depth +
// forward+ + (optional RT shadow) + transparent passes.
void render(Pipeline*, const View&);

} // namespace psynder::render::pipeline
