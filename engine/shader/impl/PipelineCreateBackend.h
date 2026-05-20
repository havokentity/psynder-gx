// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/impl/PipelineCreateBackend.h
//
// Internal header — declares the backend-specific pipeline-state-object
// construction routines that lane 08 calls AFTER slangc emits SPIR-V
// (Vulkan) or Metal IR (Metal) bytes.
//
// Owned by lane 08. Not part of the public ABI. Other lanes never
// include this file.
//
// Why these live here (and not inside ShaderCompile.cpp): each backend
// requires its own toolchain headers (Vulkan: <volk.h>, Metal: <Metal/Metal.h>).
// Splitting into per-backend TUs keeps ShaderCompile.cpp free of GPU-API
// includes and lets the build system gate the .mm vs .cpp TU on the
// active backend define.
//
// ─── Lane-07 handshake (extern "C" shims lane 07 published) ──────────────
// After a pipeline state object is constructed in lane 08, it must be
// registered with lane 07's bind_pipeline shim so the encoder API can
// resolve PipelineHandle::id → VkPipeline / MTLRenderPipelineState.
//
//   psynder_gx_vk_register_pipeline (id, VkPipeline*, VkPipelineLayout*, bind_point)
//   psynder_gx_mtl_register_render_pso (id, MTLRenderPipelineState*)
//   psynder_gx_mtl_register_compute_pso(id, MTLComputePipelineState*, tgs_x,y,z)
//
// All shims live in lane 07. Their prototypes are forward-declared inside
// the per-backend TU below so lane 08 does not have to drag lane-07
// internals into its compile.

#pragma once

#include "shader/impl/ShaderImpl.h"

#include <cstdint>
#include <vector>

namespace psynder::shader::impl {

// ─── M1 default vertex layout (lane09-004 follow-up) ─────────────────────
// PublicShader.h's GraphicsPipelineDesc cannot grow vertex-layout fields
// without breaking the frozen ABI contract. For M1 we hard-code an
// interleaved layout that matches what lane 09's forward.slang and the
// passthrough.slang built-in expect:
//
//   binding 0, stride 32 bytes:
//     +0  : float3 position
//     +12 : float3 normal
//     +24 : float2 uv
//
// Lane 09 + lane 12 publish meshes already laid out this way. The proper
// flexible variant lives behind a new opaque vertex-layout struct that
// PublicShader.h will gain a setter for in a follow-up (Issue lane09-004
// already filed against the orchestrator).
struct DefaultVertexLayout {
    static constexpr std::uint32_t kStrideBytes = 32;
    static constexpr std::uint32_t kAttribCount = 3;
    // attribute (offset_bytes, component_count) tuples, in slot order
    static constexpr std::uint32_t kAttribOffsets[3] = { 0,  12, 24 };
    static constexpr std::uint32_t kAttribCompCnt[3] = { 3,   3,  2 };
};

// ─── Construct + register a graphics pipeline state object ──────────────
// Called immediately after compile_to_spirv (Vulkan) or compile_to_metal_ir
// (Metal) succeeds. The implementation lives in either
// PipelineCreateVulkan.cpp or PipelineCreateMetal.mm depending on the
// active backend.
//
// `handle_id`: the monotonic id reserved for this pipeline. The function
// constructs the API-native PSO and forwards (handle_id, native_pso) into
// lane 07's registry via the extern "C" shim. After this returns true,
// gpu::bind_pipeline(PipelineHandle{handle_id}) resolves to a real
// VkPipeline / MTLRenderPipelineState.
//
// On failure: logs to stderr, returns false. The handle id stays
// "unregistered" so lane 07's bind_pipeline path falls back to the
// already-implemented "not registered, draws no-op" behavior — no crash.
bool create_and_register_graphics_pso(
    std::uint32_t                     handle_id,
    const std::vector<std::uint8_t>&  vs_blob,
    const std::vector<std::uint8_t>&  fs_blob,
    const char*                       vs_entry,
    const char*                       fs_entry);

bool create_and_register_compute_pso(
    std::uint32_t                     handle_id,
    const std::vector<std::uint8_t>&  cs_blob,
    const char*                       cs_entry);

// Discard a registered PSO. Best-effort — lane 07's shim is append-only
// in its initial form, so for M1 we simply leak the API resource (the
// PSO outlives the lane-08 handle by design — destroy_pipeline only
// removes it from the lane-08 cache). Proper retire-with-frames-in-flight
// lifecycle is M2 work coordinated with lane 07's reaper.
void release_registered_pso(std::uint32_t handle_id);

} // namespace psynder::shader::impl
