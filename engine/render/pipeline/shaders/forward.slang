// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/shaders/forward.slang
//
// Lane 09 — Forward+ opaque pass shader (M2 PBR metallic-roughness).
//
// Vertex layout matches Pipeline_internal::Vertex:
//   POSITION (3) + NORMAL (3) + TEXCOORD0 (2) + COLOR0 (4)
//
// Bindings (logical — actual descriptor-set/index mapping is owned by the
// lane 09 pipeline-layout code, which is not in scope at scaffold time):
//   slot 0 (UBO)        : per-view (view_proj, camera_pos, light_count)
//   slot 1 (UBO)        : per-draw (world_matrix, material_id)
//   slot 2 (StructBuf)  : light_buffer    (Light[256])
//   slot 3 (StructBuf)  : cluster_light_buffer (per-cluster (offset, count) + index list)
//   slot 4 (Tex2D)      : u_albedo
//   slot 5 (Tex2D)      : u_normal
//   slot 6 (Tex2D)      : u_metallic_roughness  (R = metallic, G = roughness)
//   slot 7 (Sampler)    : u_linear_sampler
//
// Compile (M2):
//   slangc engine/render/pipeline/shaders/forward.slang \
//       -entry vs_main -stage vertex   -profile glsl_450 -target spirv \
//       -o forward.vs.spv
//   slangc engine/render/pipeline/shaders/forward.slang \
//       -entry fs_main -stage fragment -profile glsl_450 -target spirv \
//       -o forward.fs.spv
//
// Lane 08's create_graphics() invokes slangc for us; we just specify the
// path + entry points. See PublicShader.h.

struct Light {
    float3 position_ws;   // world-space position (point light)
    float  range;         // attenuation cutoff (metres)
    float3 color;         // linear RGB, pre-multiplied by intensity
    float  pad;
};

cbuffer PerView : register(b0) {
    float4x4 g_view_proj;
    float4   g_camera_pos_ws;   // .xyz = camera position, .w = light_count
};

cbuffer PerDraw : register(b1) {
    float4x4 g_world_matrix;
    uint     g_material_id;
    uint3    g_pad;
};

StructuredBuffer<Light>   g_lights              : register(t2);
StructuredBuffer<uint2>   g_cluster_offset_count: register(t3); // (offset, count)
StructuredBuffer<uint>    g_cluster_light_index : register(t4);

Texture2D    u_albedo             : register(t5);
Texture2D    u_normal             : register(t6);
Texture2D    u_metallic_roughness : register(t7);
SamplerState u_linear_sampler     : register(s0);

// ─── Vertex ──────────────────────────────────────────────────────────────
struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct VSOutput {
    float4 sv_pos      : SV_Position;
    float3 position_ws : TEXCOORD0;
    float3 normal_ws   : TEXCOORD1;
    float2 uv          : TEXCOORD2;
    float4 color       : COLOR0;
};

[shader("vertex")]
VSOutput vs_main(VSInput vin) {
    VSOutput vout;
    float4 pos_ws = mul(g_world_matrix, float4(vin.position, 1.0f));
    vout.position_ws = pos_ws.xyz;
    vout.sv_pos      = mul(g_view_proj, pos_ws);
    // Normal transformed by 3x3 of world matrix (no non-uniform scale at
    // scaffold time; full inverse-transpose path comes in M5).
    vout.normal_ws = normalize(mul((float3x3)g_world_matrix, vin.normal));
    vout.uv        = vin.uv;
    vout.color     = vin.color;
    return vout;
}

// ─── PBR helpers (Cook-Torrance + Smith GGX) ─────────────────────────────

static const float kPI = 3.14159265359f;

float distribution_ggx(float n_dot_h, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float n2 = n_dot_h * n_dot_h;
    float denom = (n2 * (a2 - 1.0f) + 1.0f);
    return a2 / (kPI * denom * denom + 1e-6f);
}

float geometry_schlick_ggx(float n_dot_v, float roughness) {
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return n_dot_v / (n_dot_v * (1.0f - k) + k + 1e-6f);
}

float geometry_smith(float n_dot_v, float n_dot_l, float roughness) {
    return geometry_schlick_ggx(n_dot_v, roughness)
         * geometry_schlick_ggx(n_dot_l, roughness);
}

float3 fresnel_schlick(float cos_theta, float3 f0) {
    return f0 + (1.0f - f0) * pow(1.0f - cos_theta, 5.0f);
}

// ─── Cluster lookup ──────────────────────────────────────────────────────
//
// SCAFFOLD: we'd derive cluster index from gl_FragCoord + projection.
// At M2 ship time this is replaced by the canonical clustered-lighting
// lookup from DESIGN §7.3. For the scaffold we iterate every light.
uint2 cluster_for_fragment(float4 sv_pos) {
    // (offset, count) — scaffold: 0 lights from the cluster buffer.
    return uint2(0, 0);
}

// ─── Fragment ────────────────────────────────────────────────────────────
[shader("fragment")]
float4 fs_main(VSOutput fin) : SV_Target {
    // Sample material textures. The default sampler (u_linear_sampler) is
    // trilinear; lane 21 / 09 wires per-material samplers later.
    float3 albedo   = u_albedo.Sample(u_linear_sampler, fin.uv).rgb * fin.color.rgb;
    float3 mr       = u_metallic_roughness.Sample(u_linear_sampler, fin.uv).rgb;
    float  metallic  = mr.r;
    float  roughness = max(0.04f, mr.g);

    // Tangent-space normal mapping omitted at scaffold; use interpolated
    // world normal directly. Full TBN basis arrives with M5 / lane 05
    // mesh cooker work.
    float3 N = normalize(fin.normal_ws);
    float3 V = normalize(g_camera_pos_ws.xyz - fin.position_ws);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float3 lo = float3(0, 0, 0);

    // Scaffold: iterate all lights up to light_count (clamped to 256 to
    // bound the inner loop in case the constant buffer is uninitialised
    // at first frame).
    uint light_count = min((uint)g_camera_pos_ws.w, 256u);
    for (uint i = 0; i < light_count; ++i) {
        Light L = g_lights[i];
        float3 to_l = L.position_ws - fin.position_ws;
        float  dist = length(to_l);
        if (dist > L.range) continue;

        float3 Ldir = to_l / max(dist, 1e-4f);
        float3 H    = normalize(V + Ldir);

        float n_dot_l = max(dot(N, Ldir), 0.0f);
        float n_dot_v = max(dot(N, V),    0.0f);
        float n_dot_h = max(dot(N, H),    0.0f);
        float v_dot_h = max(dot(V, H),    0.0f);

        // Inverse-square attenuation with a smooth-step cut-off at range.
        float att   = 1.0f / (dist * dist + 1e-3f);
        float falloff = saturate(1.0f - (dist / L.range));
        float3 radiance = L.color * att * falloff;

        float  D = distribution_ggx(n_dot_h, roughness);
        float  G = geometry_smith(n_dot_v, n_dot_l, roughness);
        float3 F = fresnel_schlick(v_dot_h, F0);

        float3 spec = (D * G) * F / max(4.0f * n_dot_v * n_dot_l, 1e-4f);
        float3 kS = F;
        float3 kD = (float3(1, 1, 1) - kS) * (1.0f - metallic);

        lo += (kD * albedo / kPI + spec) * radiance * n_dot_l;
    }

    // Crude ambient term (M2 scaffold; DDGI replaces this in M5).
    float3 ambient = albedo * 0.03f;

    float3 color = ambient + lo;
    // Sample colour buffer is sRGB; we output linear and let the
    // hardware sRGB write convert. ACES tonemap is lane 11's post path.
    return float4(color, 1.0f);
}
