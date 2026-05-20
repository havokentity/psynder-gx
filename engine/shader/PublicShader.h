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

// ─── Vertex input layout (lane 09, closes sample01-003) ──────────────────
//
// GraphicsPipelineDesc::vertex_input lets the caller declare the vertex
// attribute layout that the backend wraps into a
// VkPipelineVertexInputStateCreateInfo (Vulkan) or MTLVertexDescriptor
// (Metal). An empty VertexInputDesc (attr_count == 0) means
// "use the lane 08 default layout": float3 position + float3 normal +
// float2 uv, interleaved at stride 32 in buffer slot 0. All existing
// callsites pre-lane-09 continue to work without source edits.
//
// DOTS contract: every type below is a POD aggregate. No virtuals, no
// allocations, no shared_ptr; small fixed-size arrays keep the desc
// trivially copyable + memset-zero-initable so the build-time pipeline
// path stays worker-thread-callable under lane 04's JobSystem.

enum class VertexAttrSemantic : std::uint8_t {
    Position,      // float3
    Normal,        // float3
    Tangent,       // float4 (xyz + handedness w)
    Color0,        // float4 (linear RGBA)
    TexCoord0,     // float2
    TexCoord1,     // float2
    JointIndex,    // u8x4   (skinning, M3+)
    JointWeight,   // float4 (skinning, M3+)
    InstanceModel, // float4 — caller fans this across 4 attributes for a mat4
};

enum class VertexAttrFormat : std::uint8_t {
    Float32,        // 1 × f32     (4 bytes)
    Float32x2,      // 2 × f32     (8 bytes)
    Float32x3,      // 3 × f32     (12 bytes)
    Float32x4,      // 4 × f32     (16 bytes)
    Uint8x4Norm,    // 4 × u8 → float in [0,1] (4 bytes)
    Uint16x2,       // 2 × u16     (4 bytes)
};

enum class VertexInputRate : std::uint8_t {
    Vertex,    // per-vertex (default for static meshes)
    Instance,  // per-instance (for indirect/instanced draw)
};

struct VertexAttr {
    VertexAttrSemantic semantic         = VertexAttrSemantic::Position;
    VertexAttrFormat   format           = VertexAttrFormat::Float32x3;
    std::uint8_t       buffer_slot      = 0;  // which bound vertex buffer holds it
    // u16 — Vulkan's maxVertexInputAttributeOffset is typically ≥2047 and
    // Metal allows offsets up to the binding stride.  A u8 cap of 255 was
    // too restrictive for any layout with more than a handful of float4
    // attributes (pos+normal+tangent+uv0+uv1+color+joint_idx+joint_weight
    // would near or exceed 255B by M2's skinned-mesh path).  Copilot PR #16
    // review caught the limit BEFORE it became a frozen ABI.
    std::uint16_t      offset_in_buffer = 0;  // byte offset within that buffer's stride
};

struct VertexBufferBinding {
    // u16 — same rationale as VertexAttr::offset_in_buffer above.
    // Vulkan's maxVertexInputBindingStride is typically ≥2048; the
    // u8 cap of 255 made wide skinned-mesh / per-instance-mat4 layouts
    // impossible to express.
    std::uint16_t    stride     = 0;                       // bytes per element
    VertexInputRate  input_rate = VertexInputRate::Vertex;
};

struct VertexInputDesc {
    static constexpr std::size_t kMaxAttrs    = 16;
    static constexpr std::size_t kMaxBindings = 8;
    std::uint8_t        attr_count    = 0;
    std::uint8_t        binding_count = 0;
    VertexAttr          attrs[kMaxAttrs]        = {};
    VertexBufferBinding bindings[kMaxBindings]  = {};
};

// Byte size of a VertexAttrFormat. Used by tests + the backend builders
// to verify a sane offset/stride layout. constexpr so it is usable
// inside static_assert at the callsite for compile-time-known layouts.
constexpr std::uint32_t attr_format_size(VertexAttrFormat f) {
    switch (f) {
        case VertexAttrFormat::Float32:     return 4u;
        case VertexAttrFormat::Float32x2:   return 8u;
        case VertexAttrFormat::Float32x3:   return 12u;
        case VertexAttrFormat::Float32x4:   return 16u;
        case VertexAttrFormat::Uint8x4Norm: return 4u;
        case VertexAttrFormat::Uint16x2:    return 4u;
    }
    return 0u;
}

// Returns true if every attr in `d`:
//   * references a binding slot < binding_count, AND
//   * fits inside that binding's stride at its declared offset
//     (`offset_in_buffer + attr_format_size(format) <= bindings[slot].stride`),
// AND attr_count/binding_count fit inside the fixed-size arrays.  Used by
// both the backend builders (assert) and the unit tests (REQUIRE).
//
// The offset+size-vs-stride check (added per Copilot PR #16 review)
// catches caller bugs like "offset_in_buffer=24 on a Float32x4 attr in
// a 32-byte stride binding" — previously such layouts passed validate()
// and silently produced an out-of-bounds attribute that only the Vulkan
// validation layer / Metal runtime caught at draw time.  A binding with
// stride==0 (single-attribute densely-packed buffer) is treated as
// "stride equals the sum of attr sizes" so any single full-buffer
// attribute still validates.
constexpr bool validate(const VertexInputDesc& d) {
    if (d.attr_count    > VertexInputDesc::kMaxAttrs)    return false;
    if (d.binding_count > VertexInputDesc::kMaxBindings) return false;
    // attr_count == 0 is the "use default layout" sentinel — vacuously valid.
    if (d.attr_count == 0) return true;
    if (d.binding_count == 0) return false; // attrs need at least one binding
    for (std::uint8_t i = 0; i < d.attr_count; ++i) {
        const VertexAttr& a = d.attrs[i];
        if (a.buffer_slot >= d.binding_count) return false;
        const std::uint32_t attr_size = attr_format_size(a.format);
        if (attr_size == 0) return false;  // unmapped format enum
        const std::uint16_t stride = d.bindings[a.buffer_slot].stride;
        // stride==0 means "implicit single-attribute stride"; only valid
        // if this attr starts at offset 0 (caller is implicitly saying
        // the binding's stride equals attr_size — the backend builders
        // pick that up).
        if (stride == 0u) {
            if (a.offset_in_buffer != 0u) return false;
            continue;
        }
        if (static_cast<std::uint32_t>(a.offset_in_buffer) + attr_size
            > static_cast<std::uint32_t>(stride)) {
            return false;
        }
    }
    return true;
}

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
    // Vertex input layout. Optional — when `vertex_input.attr_count == 0`
    // the backend builders fall back to the lane 08 default layout
    // (float3 pos + float3 normal + float2 uv, single buffer slot 0,
    // stride 32). Set this explicitly to declare a non-default layout
    // (e.g. per-instance mat4 transforms, skinned meshes, multi-stream).
    // Closes sample01-003 / lane09-004.
    VertexInputDesc vertex_input    = {};
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
