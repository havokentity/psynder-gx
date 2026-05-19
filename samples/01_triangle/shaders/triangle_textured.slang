// SPDX-License-Identifier: MIT OR Apache-2.0
//
// samples/01_triangle/shaders/triangle_textured.slang — M1 deliverable.
//
// Textured triangle that rotates around the world-space Y axis. Mirrors
// the vertex layout used by lane 09's Pipeline_internal::Vertex (and the
// orchestrator's "DefaultVertexLayout" reference):
//
//   POSITION (float3) + NORMAL (float3) + TEXCOORD0 (float2) + COLOR0 (float4)
//
// 12 floats per vertex = 48 bytes stride. The brief mentions "interleaved
// float3 pos + float3 normal + float2 uv, stride 32" — that omits the
// COLOR0 attribute that lane 09 publishes in its forward.slang and
// builtin/passthrough.slang. We follow the existing engine convention
// (stride 48) so the sample slots cleanly into the M2 forward+ path when
// lane 09 ships the canonical PBR pipeline.
//
// Push-constant block layout (lane 07's kMaxPushConstantBytes = 128):
//   bytes  0..63  : float4x4 g_mvp  — model-view-projection (column-major)
//   bytes 64..127 : reserved padding (kept zero by the sample; allows future
//                                     normal_matrix / per-draw uniforms
//                                     without an ABI break).
//
// Lane 07 binds the push-constant block to:
//   * Vulkan : VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
//              via vkCmdPushConstants (slot 0, offset 0, size 128).
//   * Metal  : setVertexBytes / setFragmentBytes at buffer index 0 (see
//              kPushConstantSlot in mtl/MetalBackend.mm). Buffers from
//              bind_vertex_buffer(binding=0) live at index 1 (see
//              kVertexBufferSlot0).
//
// Texture binding:
//   * Slot 0 — sampled 2D RGBA8 (24x24 checkerboard baked into the sample).
//   * Default trilinear sampler from psynder::gpu::create_sampler().
//
// Compile (driven by lane 08's create_graphics):
//   slangc samples/01_triangle/shaders/triangle_textured.slang \
//       -entry vs_main -stage vertex   -profile glsl_450 -target spirv \
//       -o /tmp/triangle_textured_vs.spv
//   slangc samples/01_triangle/shaders/triangle_textured.slang \
//       -entry fs_main -stage fragment -profile glsl_450 -target spirv \
//       -o /tmp/triangle_textured_fs.spv
//
// Acceptance: PSO compile must succeed on the host's slangc 2026.1 (the
// shader-cooker path in engine/shader/ShaderCompile.cpp invokes slangc as
// a subprocess at runtime — the M1 demo only needs compile success, the
// actual on-screen visual is gated by lane 07 publishing a Metal PSO for
// the resulting PipelineHandle).

struct PushConstants {
    float4x4 mvp;
};

// Slang convention: push-constants on Vulkan land in a uniform block
// decorated with [[vk::push_constant]]; on Metal they map to the constant
// buffer at the reserved push-constant slot (see PublicGpu.h push_constants
// + the per-backend convention in MetalBackend.mm / VulkanBackend.cpp).
[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;

// Texture + default sampler — slot 0 in the descriptor set / Metal buffer
// table that lane 07 ships post-push-constants.
Texture2D    u_texture       : register(t0);
SamplerState u_linear_sampler : register(s0);

// ─── Vertex ──────────────────────────────────────────────────────────────
//
// Inputs match Pipeline_internal::Vertex / DefaultVertexLayout:
//   POSITION (float3) | NORMAL (float3) | TEXCOORD0 (float2) | COLOR0 (float4)
//
// The COLOR0 attribute is wired through to fs_main so the M1 demo's
// per-vertex RGB tint visibly modulates the texture sample (which is
// also how lane 09's passthrough.slang produced its triangle output).
struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct VSOutput {
    float4 sv_pos : SV_Position;
    float2 uv     : TEXCOORD0;
    float4 color  : COLOR0;
};

[shader("vertex")]
VSOutput vs_main(VSInput vin)
{
    VSOutput vout;
    vout.sv_pos = mul(g_push.mvp, float4(vin.position, 1.0f));
    vout.uv     = vin.uv;
    vout.color  = vin.color;
    return vout;
}

// ─── Fragment ────────────────────────────────────────────────────────────
//
// Outputs the texture sample modulated by the per-vertex colour tint.
// At runtime the colour is what proves both the vertex stream + push
// constants reached the rasteriser; the texture sample is what proves
// the descriptor binding + sampler creation reached the fragment shader.
[shader("fragment")]
float4 fs_main(VSOutput fin) : SV_Target
{
    float4 tex_color = u_texture.Sample(u_linear_sampler, fin.uv);
    return tex_color * fin.color;
}
