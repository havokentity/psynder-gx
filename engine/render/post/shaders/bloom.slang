// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/shaders/bloom.slang
//
// Lane 11 — Bloom post-process.
//
// Two-pass approach:
//   Pass 0  (cs_threshold)  — downsample scene color, extract bright pixels
//                              above threshold, write to a half-res HDR buffer.
//   Pass 1  (cs_blur_h)     — separable Gaussian horizontal blur on the
//                              threshold buffer.
//   Pass 2  (cs_blur_v)     — separable Gaussian vertical blur, producing the
//                              final bloom mask.
//   Pass 3  (fs_composite)  — fullscreen add: scene + bloom_mask * strength.
//
// M2 quality tier: 5-tap Gaussian, single mip level.
// Future: dual-kawase pyramid, multiple mip levels, temporal stability.
//
// Slang compiles to SPIR-V (Vulkan) and Metal IR (macOS) via psy::shader.
// Invoked by PostProcess.cpp through psy::shader::create_compute /
// psy::shader::create_graphics.

// ── Shared uniform buffer ─────────────────────────────────────────────────
struct BloomParams
{
    float threshold;       // luminance threshold for bright-pass (default 1.0)
    float strength;        // additive blend weight (maps to PostDesc::bloom_strength)
    uint  src_width;       // source texture width
    uint  src_height;      // source texture height
};

ParameterBlock<BloomParams> g_bloom;

// ── Textures ──────────────────────────────────────────────────────────────
Texture2D<float4>    t_scene_color;     // linear HDR input (Rgba16Float)
RWTexture2D<float4>  t_bloom_write;     // half-res bright-pass or blur scratch
Texture2D<float4>    t_bloom_read;      // bloom mask after both blur passes
SamplerState         s_linear;          // bilinear clamp, set by host

// ── Helpers ───────────────────────────────────────────────────────────────

float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// 5-tap half-Gaussian weights (symmetric).
static const float k_gauss[3] = { 0.38774, 0.24477, 0.06136 };

// ── Pass 0: threshold + downsample (compute) ──────────────────────────────
// Dispatched at half-res: one thread per output pixel.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_threshold(uint3 tid : SV_DispatchThreadID)
{
    uint2 dst = tid.xy;
    uint  half_w = (g_bloom.src_width  + 1u) >> 1u;
    uint  half_h = (g_bloom.src_height + 1u) >> 1u;
    if (dst.x >= half_w || dst.y >= half_h) return;

    // 2x2 box sample to pick the maximum luminance texel.
    float2 uv_base = (float2(dst) * 2.0 + 1.0) / float2(g_bloom.src_width, g_bloom.src_height);
    float2 tx = float2(1.0 / float(g_bloom.src_width), 1.0 / float(g_bloom.src_height));

    float4 c00 = t_scene_color.SampleLevel(s_linear, uv_base,              0);
    float4 c10 = t_scene_color.SampleLevel(s_linear, uv_base + float2(tx.x, 0),  0);
    float4 c01 = t_scene_color.SampleLevel(s_linear, uv_base + float2(0,  tx.y), 0);
    float4 c11 = t_scene_color.SampleLevel(s_linear, uv_base + tx,              0);

    float4 avg = (c00 + c10 + c01 + c11) * 0.25;

    // Soft knee: max(0, lum - threshold) ramp avoids hard cutoff aliasing.
    float lum   = luminance(avg.rgb);
    float ramp  = max(0.0, lum - g_bloom.threshold);
    float scale = (lum > 1e-5) ? ramp / lum : 0.0;

    t_bloom_write[dst] = float4(avg.rgb * scale, 1.0);
}

// ── Pass 1: horizontal Gaussian blur (compute) ────────────────────────────
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_blur_h(uint3 tid : SV_DispatchThreadID)
{
    uint2 dst = tid.xy;
    uint  half_w = (g_bloom.src_width  + 1u) >> 1u;
    uint  half_h = (g_bloom.src_height + 1u) >> 1u;
    if (dst.x >= half_w || dst.y >= half_h) return;

    float2 inv  = float2(1.0 / float(half_w), 1.0 / float(half_h));
    float2 uv   = (float2(dst) + 0.5) * inv;

    float4 col = t_bloom_read.SampleLevel(s_linear, uv, 0) * k_gauss[0];
    for (int i = 1; i <= 2; i++)
    {
        float2 off = float2(float(i) * inv.x, 0.0);
        col += (t_bloom_read.SampleLevel(s_linear, uv + off, 0) +
                t_bloom_read.SampleLevel(s_linear, uv - off, 0)) * k_gauss[i];
    }
    t_bloom_write[dst] = col;
}

// ── Pass 2: vertical Gaussian blur (compute) ──────────────────────────────
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_blur_v(uint3 tid : SV_DispatchThreadID)
{
    uint2 dst = tid.xy;
    uint  half_w = (g_bloom.src_width  + 1u) >> 1u;
    uint  half_h = (g_bloom.src_height + 1u) >> 1u;
    if (dst.x >= half_w || dst.y >= half_h) return;

    float2 inv  = float2(1.0 / float(half_w), 1.0 / float(half_h));
    float2 uv   = (float2(dst) + 0.5) * inv;

    float4 col = t_bloom_read.SampleLevel(s_linear, uv, 0) * k_gauss[0];
    for (int i = 1; i <= 2; i++)
    {
        float2 off = float2(0.0, float(i) * inv.y);
        col += (t_bloom_read.SampleLevel(s_linear, uv + off, 0) +
                t_bloom_read.SampleLevel(s_linear, uv - off, 0)) * k_gauss[i];
    }
    t_bloom_write[dst] = col;
}

// ── Pass 3: fullscreen composite (graphics) ───────────────────────────────
// Used with a unit-quad vertex buffer; the vertex shader emits a full-screen
// triangle (clip-space pos from vertex id, no VB needed).

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VSOut vs_bloom_composite(uint vid : SV_VertexID)
{
    // Full-screen triangle trick: 3 vertices cover the NDC quad.
    float2 pos2 = float2((vid & 1u) ? 3.0 : -1.0,
                         (vid & 2u) ? 3.0 : -1.0);
    VSOut o;
    o.pos = float4(pos2, 0.0, 1.0);
    o.uv  = pos2 * float2(0.5, -0.5) + 0.5;
    return o;
}

[shader("fragment")]
float4 fs_bloom_composite(VSOut i) : SV_Target
{
    float4 scene = t_scene_color.Sample(s_linear, i.uv);
    float4 bloom = t_bloom_read.Sample(s_linear, i.uv);
    return float4(scene.rgb + bloom.rgb * g_bloom.strength, scene.a);
}
