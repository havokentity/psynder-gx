// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/shaders/hiz_build.slang
//
// Lane 09 — HiZ pyramid downsample (compute).
//
// Reads mip N of the HiZ pyramid (or the source depth texture at N=0)
// and writes the conservative-max depth (or min, dep. on convention) into
// mip N+1. Dispatched once per mip level.
//
// Convention: depth is in [0, 1] with 0 = near, 1 = far (reverse-Z would
// flip; we adopt standard depth here for the scaffold). The cull pass
// uses MAX to be conservative for occlusion (a pixel is visible if any
// of its 2×2 source pixels is closer than the test depth).
//
// SCAFFOLD: the writes use a single binding; the real impl walks the
// mip chain via a bind-per-mip pattern (or descriptor indexing into a
// mip-sliced texture-view array). This compiles + sanity-checks the
// HLSL/Slang surface; lane 09's M3 finishes the encode plumbing.

// Push-constants — must match the layout of HizPushConstants in
// engine/render/pipeline/Hiz.cpp (4 × u32 = 16 bytes).  Vulkan honours the
// [[vk::push_constant]] decoration directly; Metal lowers it to the
// reserved push-constant buffer slot via slangc.
struct HizParams {
    uint g_src_mip;
    uint g_dst_mip;
    uint g_src_width;
    uint g_src_height;
};
[[vk::push_constant]] ConstantBuffer<HizParams> g_pc;

Texture2D<float>   g_src_depth : register(t1);   // mip N or pre-pass depth
RWTexture2D<uint>  g_dst_hiz   : register(u2);   // mip N+1 in pyramid

[numthreads(8, 8, 1)]
[shader("compute")]
void cs_downsample(uint3 did : SV_DispatchThreadID) {
    uint2 dst = did.xy;
    uint2 src = dst * 2u;
    if (src.x >= g_pc.g_src_width || src.y >= g_pc.g_src_height) return;

    // Sample 2×2 source region, take the conservative-max for occlusion.
    float d00 = g_src_depth.Load(int3(src + uint2(0, 0), 0));
    float d10 = g_src_depth.Load(int3(src + uint2(1, 0), 0));
    float d01 = g_src_depth.Load(int3(src + uint2(0, 1), 0));
    float d11 = g_src_depth.Load(int3(src + uint2(1, 1), 0));
    float dmax = max(max(d00, d10), max(d01, d11));

    // Pack to R32Uint via asuint(asfloat() round-trip would be the same
    // bit pattern; we store the raw bits so the cull pass can read with
    // a cheap reinterpret).
    g_dst_hiz[dst] = asuint(dmax);
}
