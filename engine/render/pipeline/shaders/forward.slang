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

// Push-constants — must match the layout of OpaquePushConstants in
// engine/render/pipeline/OpaquePass.cpp (2 × float4x4 = 128 bytes, which
// is also kMaxPushConstantBytes).  This replaces the prior b0 PerView +
// b1 PerDraw cbuffers — on Vulkan, push_constants don't populate b0/b1
// at all, and on Metal lane 07's backend reserves the push slot, so the
// only path that actually delivers the matrices today is push_constants.
//
// Fields NOT in the push range (camera_pos, light_count, material_id) are
// SCAFFOLD-removed below until M3 lands a per-view UBO via lane 07's
// descriptor-set API.  The fragment shader is downgraded to an un-lit
// passthrough until that happens (see fs_main).
struct OpaqueParams {
    float4x4 g_view_proj;
    float4x4 g_world_matrix;
};
[[vk::push_constant]] ConstantBuffer<OpaqueParams> g_pc;

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
    float4 pos_ws = mul(g_pc.g_world_matrix, float4(vin.position, 1.0f));
    vout.position_ws = pos_ws.xyz;
    vout.sv_pos      = mul(g_pc.g_view_proj, pos_ws);
    // Normal transformed by 3x3 of world matrix (no non-uniform scale at
    // scaffold time; full inverse-transpose path comes in M5).
    vout.normal_ws = normalize(mul((float3x3)g_pc.g_world_matrix, vin.normal));
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
//
// SCAFFOLD: the full PBR loop iterated `g_camera_pos_ws.w` lights and used
// `g_camera_pos_ws.xyz` for the view vector.  Both fields live in the M3
// per-view UBO, NOT in push_constants — so until lane 07 grows the
// descriptor-set API + lane 09 wires a per-view UBO, the body is
// downgraded to an un-lit albedo passthrough.  The PBR helpers above
// (distribution_ggx / geometry_smith / fresnel_schlick) are kept compiled
// so the M3 re-enable is a single function-body swap, no header churn.
[shader("fragment")]
float4 fs_main(VSOutput fin) : SV_Target {
    float3 albedo = u_albedo.Sample(u_linear_sampler, fin.uv).rgb * fin.color.rgb;
    // Subtle N·V hint so a vertical-normal triangle still shows some
    // facing variation when no lights are present.
    float3 N = normalize(fin.normal_ws);
    float facing = saturate(0.5f + 0.5f * N.z);
    float3 color = albedo * (0.6f + 0.4f * facing);
    return float4(color, 1.0f);
}
