// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/shaders/depth_only.slang
//
// Lane 09 — depth pre-pass shader (no color writes).
//
// DESIGN-PSYNDER-GX.md §7.1 step 3 — depth pre-pass lays down a Depth32Float
// target that the opaque forward+ pass then samples with depth-equal compare
// and the GPU cull pass downsamples into the HiZ pyramid.
//
// Vertex layout matches Pipeline_internal::Vertex:
//   POSITION (3) + NORMAL (3) + TEXCOORD0 (2) + COLOR0 (4)
// Only POSITION is consumed here; the remaining attributes are present in
// the bound vertex buffer but ignored by the fragment-less depth pipeline.
//
// Push-constant layout (lane 09 encoder convention — see Pipeline.cpp):
//   float4x4 view_proj
//   float4x4 world_matrix
// Total: 128 bytes — exactly fits the kMaxPushConstantBytes budget.

struct DepthPushConstants {
    float4x4 view_proj;
    float4x4 world_matrix;
};

[[vk::push_constant]] DepthPushConstants pc;

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct VSOutput {
    float4 sv_pos : SV_Position;
};

[shader("vertex")]
VSOutput vs_main(VSInput vin) {
    VSOutput vout;
    float4 pos_ws = mul(pc.world_matrix, float4(vin.position, 1.0f));
    vout.sv_pos   = mul(pc.view_proj, pos_ws);
    return vout;
}

// A no-color fragment entry is enough for the depth-only target. We still
// declare an empty fragment because some backends (Vulkan + Slang's
// fragment-less-pipeline path) need an explicit entry name to build the
// pipeline. Returning SV_Depth would let us also override depth; we don't.
[shader("fragment")]
void fs_main(VSOutput /*fin*/) {
    // Empty — depth is written from sv_pos.z. No color attachment is bound.
}
