// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/shaders/tonemap.slang
//
// Lane 11 - ACES filmic tonemapper (Narkowicz 2015 approximation) + exposure.
//
// Pipeline:
//   1. Apply camera exposure in EV: linear_scene *= exp2(exposure_ev).
//      Positive EV brightens, negative EV darkens (matches photography
//      stops). Applied BEFORE the ACES curve so the curve's shoulder/toe
//      respond to exposure -- the intuitive behaviour the artists expect.
//   2. ACES filmic tone-map (Krzysztof Narkowicz 2015 approximation):
//          color = saturate( (x*(a*x+b)) / (x*(c*x+d)+e) )
//      with a=2.51, b=0.03, c=2.43, d=0.59, e=0.14.
//   3. Output is written to a UNORM_SRGB / BGRA8Unorm_sRGB target -- the
//      swapchain's hardware OETF handles linear -> sRGB. We must NOT
//      apply a software gamma curve here; doing so would gamma-twice.
//
// References:
//   - Krzysztof Narkowicz, "ACES Filmic Tone Mapping Curve" (2015):
//     https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
//   - ACES Central / AMPAS aces-dev (RRT+ODT reference path).
//
// The post host (PostProcess.cpp) sets exposure_ev each frame via the
// uniform buffer; auto-exposure / camera EV / r_exposure cvar all funnel
// through that path.
//
// Slang compiles to SPIR-V (Vulkan) and Metal IR (macOS) via psy::shader.

// ─── Uniform block ───────────────────────────────────────────────────────
struct TonemapParams
{
    float exposure_ev; // camera EV (0 = no change; positive = brighter)
    float _pad0;
    float _pad1;
    float _pad2;
};

ParameterBlock<TonemapParams> g_tonemap;

// ─── Textures ────────────────────────────────────────────────────────────
Texture2D<float4>   t_hdr;       // linear HDR input (post-bloom-composite)
SamplerState        s_point;     // point clamp; one input texel == one output pixel

// ─── ACES tone-map (Narkowicz 2015) ──────────────────────────────────────
//
// Curve coefficients are fixed -- DO NOT change without updating the
// regression-test reference values in tests/unit/post_smoke.cpp.
float3 aces_tonemap(float3 x)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// ─── Vertex shader (full-screen triangle, no vertex buffer) ──────────────
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VSOut vs_tonemap(uint vid : SV_VertexID)
{
    float2 pos2 = float2((vid & 1u) ? 3.0f : -1.0f,
                         (vid & 2u) ? 3.0f : -1.0f);
    VSOut o;
    o.pos = float4(pos2, 0.0f, 1.0f);
    o.uv  = pos2 * float2(0.5f, -0.5f) + 0.5f;
    return o;
}

// ─── Fragment shader ─────────────────────────────────────────────────────
[shader("fragment")]
float4 fs_tonemap(VSOut i) : SV_Target
{
    // One input texel == one output pixel (no upscale/filter here).
    float4 hdr = t_hdr.Sample(s_point, i.uv);

    // 1. Exposure in EV (perceptual stops) -- apply BEFORE the curve so
    //    the shoulder/toe respond to exposure (the artist-intuitive form).
    float3 exposed = hdr.rgb * exp2(g_tonemap.exposure_ev);

    // 2. ACES tone-map (Narkowicz 2015).
    float3 mapped = aces_tonemap(exposed);

    // 3. Write linear -- the swapchain BGRA8Unorm_sRGB / Rgba8Srgb format
    //    applies the OETF in hardware on store. NO software gamma here.
    return float4(mapped, 1.0f);
}
