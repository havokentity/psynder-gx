// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/shaders/tonemap.slang
//
// Lane 11 — ACES filmic tonemapper + exposure.
//
// Pipeline:
//   1. Apply camera exposure in EV: linear_scene *= exp2(exposure_ev).
//   2. RRT (Reference Rendering Transform) approximation — Stephen Hill's
//      fitted ACES curve that closely matches the full ACES 1.0 RRT+ODT at
//      a fraction of the cost.
//   3. Linear → sRGB / Gamma 2.2 gamma correction.
//
// References:
//   - Narkowicz 2015 "ACES Filmic Tone Mapping Curve"
//   - Stephen Hill "Optimized ACES" (fit to the full 3x3 matrix RRT path)
//   - ACES Central github.com/ampas/aces-dev
//
// The psy::render::post host code calls set_exposure_ev() which updates the
// uniform buffer backing BloomParams.exposure_ev each frame.
//
// Slang compiles to SPIR-V (Vulkan) and Metal IR (macOS) via psy::shader.

// ── Uniform block ─────────────────────────────────────────────────────────
struct TonemapParams
{
    float exposure_ev; // camera EV compensation (0 = no change; neg = darker)
    float _pad0;
    float _pad1;
    float _pad2;
};

ParameterBlock<TonemapParams> g_tonemap;

// ── Textures ──────────────────────────────────────────────────────────────
Texture2D<float4>   t_hdr;       // linear HDR input (post-bloom, pre-tonemap)
SamplerState        s_point;     // point clamp; no filtering needed here

// ── ACES helpers ──────────────────────────────────────────────────────────

// Input transform matrix: converts sRGB linear to ACEScg.
// From ACES CTL: https://github.com/ampas/aces-dev
static const float3x3 k_aces_input_mat = float3x3(
     0.59719f,  0.35458f,  0.04823f,
     0.07600f,  0.90834f,  0.01566f,
     0.02840f,  0.13383f,  0.83777f
);

// Output transform matrix: converts ACEScg to sRGB linear (inverse of input).
static const float3x3 k_aces_output_mat = float3x3(
     1.60475f, -0.53108f, -0.07367f,
    -0.10208f,  1.10813f, -0.00605f,
    -0.00327f, -0.07276f,  1.07602f
);

// RRTAndODTFit — Stephen Hill's fitted ACES RRT+ODT curve.
// Optimized rational polynomial, valid for x in [0, +inf).
float3 rrt_and_odt_fit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

// Full ACES tone-map path (input: linear sRGB; output: sRGB [0,1]).
float3 aces_fitted(float3 color)
{
    color = mul(k_aces_input_mat, color);
    color = rrt_and_odt_fit(color);
    color = mul(k_aces_output_mat, color);
    return saturate(color);
}

// Linear → sRGB gamma correction.
float3 linear_to_srgb(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, float3(0.0, 0.0, 0.0)), float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    return lerp(lo, hi, step(float3(0.0031308, 0.0031308, 0.0031308), c));
}

// ── Vertex shader (full-screen triangle, no vertex buffer) ────────────────
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VSOut vs_tonemap(uint vid : SV_VertexID)
{
    float2 pos2 = float2((vid & 1u) ? 3.0 : -1.0,
                         (vid & 2u) ? 3.0 : -1.0);
    VSOut o;
    o.pos = float4(pos2, 0.0, 1.0);
    o.uv  = pos2 * float2(0.5, -0.5) + 0.5;
    return o;
}

// ── Fragment shader ───────────────────────────────────────────────────────
[shader("fragment")]
float4 fs_tonemap(VSOut i) : SV_Target
{
    // Point-sample: one texel = one output pixel.
    float4 hdr = t_hdr.Sample(s_point, i.uv);

    // 1. Apply exposure compensation (EV units, perceptual stops).
    float3 exposed = hdr.rgb * exp2(g_tonemap.exposure_ev);

    // 2. ACES filmic tone-map.
    float3 mapped = aces_fitted(exposed);

    // 3. Linear → sRGB gamma encode for display.
    float3 srgb = linear_to_srgb(mapped);

    return float4(srgb, 1.0);
}
