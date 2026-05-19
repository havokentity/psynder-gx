// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/shaders/bloom.slang
//
// Lane 11 - Bloom post-process: Karis 2013 progressive downsample + upsample.
//
// References:
//   - Brian Karis, "Tone Mapping" (SIGGRAPH 2013) -- the partial Karis average
//     13-tap downsample that suppresses firefly speckle on the brightest sample
//     and the 3x3 tent upsample that builds a smooth bloom kernel from a mip
//     pyramid.
//   - Jorge Jimenez, "Next Generation Post Processing in Call of Duty"
//     (SIGGRAPH 2014) -- popularised the dual-filter / progressive pyramid
//     formulation used here.
//
// Pipeline (host orchestrates one dispatch per shader):
//
//   1. cs_bloom_prefilter      mip0 (scene_color, HDR) -> mip1 (half-res)
//                              threshold + Karis 13-tap downsample.
//
//   2. cs_bloom_downsample     mip[N]   -> mip[N+1]
//                              Karis 13-tap, no threshold (already filtered).
//                              Repeated for mips 1..N-1.
//
//   3. cs_bloom_upsample       mip[N+1] -> mip[N]
//                              3x3 tent, additive blend onto destination.
//                              Repeated for mips N..1.
//
//   4. cs_bloom_composite      mip1 + scene_color -> hdr_bloom_out
//                              scene + bloom * strength.
//
// The post host (PostProcess.cpp) supplies the source/dest texture binding
// for each invocation so the same 4 entry points service the whole pyramid.
//
// Slang compiles to SPIR-V (Vulkan) and Metal IR (macOS) via psy::shader.

// ─── Shared push-constant block ──────────────────────────────────────────
//
// PostProcess.cpp pushes this struct via gpu::push_constants() before each
// dispatch -- one src_size / dst_size pair per pyramid level.
//
// Total size: 32 bytes (well under kMaxPushConstantBytes = 128).
struct BloomPush
{
    float2 src_inv_size;   // 1.0 / src_texture_size  (used to step in src UV)
    float2 dst_inv_size;   // 1.0 / dst_texture_size  (used to centre dst UV)
    uint2  dst_size;       // dst width / height in pixels (clip OOB threads)
    float  threshold;      // luminance threshold (prefilter only; >0)
    float  strength;       // composite bloom_strength (composite only)
};

[[vk::push_constant]] BloomPush g_push;

// ─── Textures ────────────────────────────────────────────────────────────
Texture2D<float4>    t_src;      // source mip (read; sampled with s_linear)
RWTexture2D<float4>  t_dst;      // destination mip (write)
SamplerState         s_linear;   // bilinear clamp, host-bound

// ─── Helpers ─────────────────────────────────────────────────────────────

// Rec.601 luminance per task prompt (dot(rgb, vec3(0.299, 0.587, 0.114))).
float luma_601(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

// Karis 2013 partial-average weight: w = 1 / (1 + Y) where Y is Rec.601 luma.
// Heavier weight on dimmer texels -- firefly-suppressing without losing
// energy from genuinely bright HDR regions.
float karis_weight(float3 c)
{
    return 1.0f / (1.0f + luma_601(c));
}

// Sample helper: src is bilinear-sampled; `uv` is in [0,1] of src texture.
float3 sample_src(float2 uv)
{
    return t_src.SampleLevel(s_linear, uv, 0).rgb;
}

// ─── Karis 13-tap downsample ─────────────────────────────────────────────
//
// Layout (numbers = tap index, '.' = empty):
//
//      0   .   1   .   2
//      .   3   .   4   .
//      5   .   6   .   7
//      .   8   .   9   .
//     10   .  11   .  12
//
// Four "inner 2x2" boxes (taps 3,4,8,9 weight 0.5) and one outer box plus
// four corner-overlap boxes (taps 0,1,2,5,6,7,10,11,12 weight 0.125).
//
// Partial Karis average: split into 5 sub-boxes, weight each by its inverse
// luminance, then sum. Suppresses single-pixel fireflies on the brightest
// box while preserving the rest.
float3 karis_downsample(float2 uv)
{
    // `uv` is the centre of the dst texel projected into src UV space.
    // src_inv_size steps one src texel; we sample taps at ±1 and ±2 texels.
    float2 e1 = g_push.src_inv_size;
    float2 e2 = g_push.src_inv_size * 2.0f;

    // 13 taps.
    float3 a = sample_src(uv + float2(-e2.x,  e2.y));   //  0
    float3 b = sample_src(uv + float2(   0.0,  e2.y));   //  1
    float3 c = sample_src(uv + float2( e2.x,  e2.y));   //  2

    float3 d = sample_src(uv + float2(-e1.x,  e1.y));   //  3
    float3 e = sample_src(uv + float2( e1.x,  e1.y));   //  4

    float3 f = sample_src(uv + float2(-e2.x,    0.0));   //  5
    float3 g = sample_src(uv);                            //  6 centre
    float3 h = sample_src(uv + float2( e2.x,    0.0));   //  7

    float3 i = sample_src(uv + float2(-e1.x, -e1.y));   //  8
    float3 j = sample_src(uv + float2( e1.x, -e1.y));   //  9

    float3 k = sample_src(uv + float2(-e2.x, -e2.y));   // 10
    float3 l = sample_src(uv + float2(   0.0, -e2.y));   // 11
    float3 m = sample_src(uv + float2( e2.x, -e2.y));   // 12

    // 5 boxes (each = avg of 4 taps).
    float3 box_centre = (d + e + i + j) * 0.25f;        // weight 0.5
    float3 box_tl     = (a + b + f + g) * 0.25f;        // weight 0.125
    float3 box_tr     = (b + c + g + h) * 0.25f;        // weight 0.125
    float3 box_bl     = (f + g + k + l) * 0.25f;        // weight 0.125
    float3 box_br     = (g + h + l + m) * 0.25f;        // weight 0.125

    // Partial Karis: weight each box by its inverse-luma, then sum.
    float w_centre = karis_weight(box_centre);
    float w_tl     = karis_weight(box_tl);
    float w_tr     = karis_weight(box_tr);
    float w_bl     = karis_weight(box_bl);
    float w_br     = karis_weight(box_br);

    // Effective per-box weights (target_w = 0.5 / 0.125 / 0.125 / 0.125 / 0.125).
    float kc = 0.5f   * w_centre;
    float kt = 0.125f * w_tl;
    float kr = 0.125f * w_tr;
    float kb = 0.125f * w_bl;
    float kk = 0.125f * w_br;

    float sum_w = kc + kt + kr + kb + kk;
    float inv_w = (sum_w > 1e-6f) ? (1.0f / sum_w) : 0.0f;

    return (box_centre * kc + box_tl * kt + box_tr * kr + box_bl * kb + box_br * kk) * inv_w;
}

// ─── 3x3 tent upsample ───────────────────────────────────────────────────
//
// Tent (triangle) filter:
//
//   1/16  2/16  1/16
//   2/16  4/16  2/16
//   1/16  2/16  1/16
//
// Reads the (smaller) src mip and accumulates into the (larger) dst mip.
// We use a *write* additive combine in the host -- host clears the dst
// only on the deepest upsample step; subsequent steps read-modify-write.
float3 tent3x3_upsample(float2 uv)
{
    float2 d = g_push.src_inv_size;
    float3 sum = float3(0, 0, 0);
    sum += sample_src(uv + float2(-d.x,  d.y)) * 1.0f;
    sum += sample_src(uv + float2(   0,  d.y)) * 2.0f;
    sum += sample_src(uv + float2( d.x,  d.y)) * 1.0f;
    sum += sample_src(uv + float2(-d.x,    0)) * 2.0f;
    sum += sample_src(uv                     ) * 4.0f;
    sum += sample_src(uv + float2( d.x,    0)) * 2.0f;
    sum += sample_src(uv + float2(-d.x, -d.y)) * 1.0f;
    sum += sample_src(uv + float2(   0, -d.y)) * 2.0f;
    sum += sample_src(uv + float2( d.x, -d.y)) * 1.0f;
    return sum * (1.0f / 16.0f);
}

// ─── Pass 1: prefilter (threshold + Karis 13-tap downsample mip0 -> mip1) ─
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_prefilter(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_push.dst_size.x || tid.y >= g_push.dst_size.y) return;

    // Centre of dst texel, in dst UV space (then identical in src UV since
    // dst is half-res of src).
    float2 uv = (float2(tid.xy) + 0.5f) * g_push.dst_inv_size;

    float3 col = karis_downsample(uv);

    // Threshold: only fragments with Rec.601 luminance > threshold contribute.
    float y     = luma_601(col);
    float ramp  = max(0.0f, y - g_push.threshold);
    float scale = (y > 1e-5f) ? (ramp / y) : 0.0f;

    t_dst[tid.xy] = float4(col * scale, 1.0f);
}

// ─── Pass 2: progressive Karis downsample (no threshold) ─────────────────
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_downsample(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_push.dst_size.x || tid.y >= g_push.dst_size.y) return;
    float2 uv = (float2(tid.xy) + 0.5f) * g_push.dst_inv_size;
    t_dst[tid.xy] = float4(karis_downsample(uv), 1.0f);
}

// ─── Pass 3: 3x3 tent upsample (additive into dst) ───────────────────────
//
// Host responsibility: the dst mip already contains the partial bloom from
// the higher-resolution upsample steps. We additively accumulate this
// (smaller) src mip on top of it.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_upsample(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_push.dst_size.x || tid.y >= g_push.dst_size.y) return;
    float2 uv  = (float2(tid.xy) + 0.5f) * g_push.dst_inv_size;
    float3 add = tent3x3_upsample(uv);
    float3 cur = t_dst[tid.xy].rgb;
    t_dst[tid.xy] = float4(cur + add, 1.0f);
}

// ─── Pass 4: composite (scene + bloom * strength) ────────────────────────
//
// Reads the mip1 bloom result via `t_src` and the scene HDR via t_scene
// (bound separately by the host). Writes the combined HDR to t_dst, which
// becomes the input to the tonemap pass.
Texture2D<float4>   t_scene;     // linear-HDR scene color (full res)

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_composite(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_push.dst_size.x || tid.y >= g_push.dst_size.y) return;
    float2 uv    = (float2(tid.xy) + 0.5f) * g_push.dst_inv_size;
    float3 scene = t_scene.SampleLevel(s_linear, uv, 0).rgb;
    float3 bloom = t_src .SampleLevel(s_linear, uv, 0).rgb;
    t_dst[tid.xy] = float4(scene + bloom * g_push.strength, 1.0f);
}
