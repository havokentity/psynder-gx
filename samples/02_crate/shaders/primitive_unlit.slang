// SPDX-License-Identifier: MIT OR Apache-2.0

struct PushConstants {
    float4x4 mvp;
    float4 material;
    float4 params;
    float4 light;
    float4 ambient_exposure;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct VSOutput {
    float4 sv_pos : SV_Position;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    float4 color  : COLOR0;
};

[shader("vertex")]
VSOutput vs_main(VSInput vin)
{
    VSOutput vout;
    vout.sv_pos = mul(g_push.mvp, float4(vin.position, 1.0f));
    vout.normal = vin.normal;
    vout.uv = vin.uv;
    vout.color = vin.color * g_push.material;
    return vout;
}

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float noise2(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p)
{
    float value = 0.0;
    float amp = 0.55;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        value += noise2(p) * amp;
        p = p * 2.07 + float2(17.1, 9.2);
        amp *= 0.52;
    }
    return value;
}

[shader("fragment")]
float4 fs_main(VSOutput fin) : SV_Target
{
    float mode = g_push.params.w;
    if (mode < -0.5) {
        float exposure = max(g_push.ambient_exposure.w, 0.05);
        float2 uv = saturate(fin.uv);
        float horizon = saturate(uv.y);
        float3 top = float3(0.075, 0.16, 0.32) + g_push.ambient_exposure.rgb * 0.18;
        float3 low = float3(0.42, 0.55, 0.72) + g_push.material.rgb * 0.08;
        float3 sky = lerp(low, top, pow(horizon, 0.62));
        float2 wind = float2(g_push.params.z, g_push.params.z * 0.37);
        float cloud = fbm(uv * float2(4.0, 2.2) + wind);
        cloud += fbm(uv * float2(9.0, 5.0) - wind * 1.7) * 0.35;
        cloud = saturate((cloud - (0.62 - g_push.params.x * 0.42)) *
                         max(g_push.params.y, 0.01) * 1.45);
        float sun_disk = pow(saturate(dot(normalize(float3(uv - 0.5, 0.38)),
                                           normalize(float3(-g_push.light.x,
                                                            -g_push.light.y * 0.55 + 0.15,
                                                            0.42)))),
                             128.0) * g_push.light.w;
        float3 cloud_rgb = lerp(float3(0.55, 0.61, 0.68), g_push.material.rgb, 0.38);
        float3 rgb = lerp(sky, cloud_rgb, cloud * 0.72);
        rgb += g_push.material.rgb * sun_disk * 0.65;
        rgb = 1.0 - exp(-rgb * exposure);
        return float4(rgb, 1.0);
    }

    float scale = max(g_push.params.x, 1.0);
    float edge = max(g_push.params.y, 0.001);
    float2 cell = frac(fin.uv * scale);
    float2 line = min(cell, 1.0 - cell);
    float grid = 1.0 - smoothstep(edge, edge * 1.8, min(line.x, line.y));
    grid *= 1.0 - saturate(abs(mode));
    float selected = g_push.params.z;
    float shade = 0.72 + grid * 0.20 + selected * 0.18;
    float alpha = saturate(g_push.material.a + grid * 0.24 + selected * 0.18);
    float3 rgb = fin.color.rgb * shade;
    if (mode > 1.5) {
        float3 n = normalize(fin.normal);
        float3 l = normalize(g_push.light.xyz);
        float ndotl = saturate(dot(n, l));
        float rough = saturate(g_push.params.x);
        float metal = saturate(g_push.params.y);
        float wrap = saturate(ndotl * (1.0 - rough * 0.35) + 0.08);
        float spec = pow(saturate(ndotl), lerp(48.0, 10.0, rough)) * lerp(0.05, 0.42, metal);
        float3 lit = g_push.ambient_exposure.rgb + wrap * g_push.light.w;
        rgb = fin.color.rgb * lit + spec.xxx;
        rgb = 1.0 - exp(-rgb * max(g_push.ambient_exposure.w, 0.05));
        alpha = 1.0;
    }
    return float4(rgb * alpha, alpha);
}
