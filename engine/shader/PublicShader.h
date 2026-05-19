// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/PublicShader.h
//
// Lane 08 — Shader compile + pipeline public-header CONTRACT (Wave 0).
//
// Slang sources → SPIR-V (Vulkan) or Metal IR (Metal) → VkPipeline /
// MTLRenderPipelineState handles. Hot reload via filesystem watcher.
//
// See DESIGN-PSYNDER-GX.md §7.7 (materials), §10.7 (asset cook).

#pragma once

#include <cstddef>
#include <cstdint>

namespace psynder::shader {

// Opaque handle to a compiled + linked pipeline (graphics or compute).
// Backed by gpu::Pipeline at the implementation level.
struct PipelineHandle { std::uint32_t id = 0; bool valid() const { return id != 0; } };

enum class Stage : std::uint8_t {
    Vertex, Fragment, Compute, Mesh, Task,
    RayGen, RayMiss, RayHitClosest, RayHitAny,
};

struct GraphicsPipelineDesc {
    const char* slang_path        = nullptr; // e.g. "shaders/forward.slang"
    const char* entry_point_vs    = "vs_main";
    const char* entry_point_fs    = "fs_main";
    // Render target formats (must match gpu::Format used at draw time)
    std::uint32_t color_format_count = 1;
    std::uint8_t  color_formats[8]   = {};
    std::uint8_t  depth_format       = 0; // Format::Depth32Float etc.
    bool          enable_depth_write = true;
    bool          enable_blend       = false;
    // Vertex layout supplied via a separate descriptor (TBD by lane 09)
};

struct ComputePipelineDesc {
    const char* slang_path     = nullptr;
    const char* entry_point_cs = "cs_main";
};

struct RtPipelineDesc {
    const char* slang_path           = nullptr;
    const char* entry_point_raygen   = nullptr;
    const char* entry_point_miss     = nullptr;
    const char* entry_point_hit_close = nullptr;
};

// Compile + create. Cached by hash of (path, entry-points, target format).
// Returns an invalid handle on failure; consult the engine console for the
// last compile log.
PipelineHandle create_graphics(const GraphicsPipelineDesc&);
PipelineHandle create_compute (const ComputePipelineDesc&);
PipelineHandle create_rt      (const RtPipelineDesc&);

void destroy_pipeline(PipelineHandle);

// Hot reload — invoked by the filesystem watcher on .slang touch. The new
// pipeline is built off-frame; in-flight frames use the old one and the
// swap is atomic between frames.
void hot_reload_changed();

} // namespace psynder::shader
