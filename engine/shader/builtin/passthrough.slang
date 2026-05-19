// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/builtin/passthrough.slang
//
// Minimal pass-through shader for toolchain validation (M1).
// Inputs:  position (clip-space) + uv + color passed through from vertex stage.
// Outputs: sampled texture colour modulated by the per-vertex colour tint.
//
// Compile targets:
//   SPIR-V (Vulkan)  — slangc -profile glsl_450 -target spirv
//   Metal IR (macOS) — slangc -profile metallib -target metallib
//
// Usage (triangle demo):
//   GraphicsPipelineDesc desc;
//   desc.slang_path      = "engine/shader/builtin/passthrough.slang";
//   desc.entry_point_vs  = "vs_main";
//   desc.entry_point_fs  = "fs_main";

struct VSInput {
    float3 position : POSITION;
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
    vout.sv_pos = float4(vin.position, 1.0f);
    vout.uv     = vin.uv;
    vout.color  = vin.color;
    return vout;
}

// ─── Fragment ────────────────────────────────────────────────────────────
uniform Sampler2D u_texture;

[shader("fragment")]
float4 fs_main(VSOutput fin) : SV_Target
{
    float4 tex_color = u_texture.Sample(fin.uv);
    return tex_color * fin.color;
}
