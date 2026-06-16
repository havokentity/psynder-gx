// SPDX-License-Identifier: MIT OR Apache-2.0
//
// GPU overlay shader. Input position is in top-left pixel space.

struct PushConstants {
    float2 viewport_rcp;
    float  time_seconds;
    float  effect_strength;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;

struct VSInput {
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct VSOutput {
    float4 sv_pos : SV_Position;
    float2 uv     : TEXCOORD0;
    float4 color  : COLOR0;
};

[[vk::binding(0, 0)]] Texture2D    u_texture        : register(t0);
[[vk::binding(1, 0)]] SamplerState u_linear_sampler : register(s0);

[shader("vertex")]
VSOutput vs_main(VSInput vin)
{
    VSOutput vout;
    const float2 ndc = float2(vin.position.x * g_push.viewport_rcp.x - 1.0f,
                              1.0f - vin.position.y * g_push.viewport_rcp.y);
    vout.sv_pos = float4(ndc, 0.0f, 1.0f);
    vout.uv = vin.uv;
    vout.color = float4(vin.color.w, vin.color.z, vin.color.y, vin.color.x);
    return vout;
}

[shader("fragment")]
float4 fs_main(VSOutput fin) : SV_Target
{
    float4 mask = u_texture.Sample(u_linear_sampler, fin.uv);
    const float px = max(fwidth(mask.r), 0.0025f);
    const float glyph_alpha = smoothstep(0.5f - px, 0.5f + px, mask.r);
    const float alpha = fin.color.a * glyph_alpha;
    float3 rgb = fin.color.rgb;
    const float fx = saturate(g_push.effect_strength);
    if (fx > 0.0f) {
        const float2 uv = fin.sv_pos.xy * g_push.viewport_rcp * 0.5f;
        const float t = g_push.time_seconds;
        const float scan = 0.5f + 0.5f * sin(uv.y * 980.0f + t * 36.0f);
        const float wave = sin((uv.x * 9.0f + uv.y * 4.0f) + t * 1.7f);
        const float grid_x = smoothstep(0.985f, 1.0f, sin((uv.x + t * 0.012f) * 70.0f) * 0.5f + 0.5f);
        const float grid_y = smoothstep(0.988f, 1.0f, sin((uv.y - t * 0.018f) * 46.0f) * 0.5f + 0.5f);
        const float grid = max(grid_x, grid_y);
        const float chroma = sin((uv.x - uv.y) * 22.0f + t * 2.4f);

        rgb += fx * float3(0.015f, 0.055f, 0.115f) * scan;
        rgb += fx * grid * float3(0.08f, 0.22f, 0.36f);
        rgb.r += fx * 0.025f * chroma;
        rgb.b -= fx * 0.018f * chroma;
        rgb *= 1.0f + fx * wave * 0.035f;
    }
    return float4(saturate(rgb) * alpha, alpha);
}
