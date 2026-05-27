// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Psynder-GX runtime intro background.
//
// Signed-distance architecture prototype for the no-scene/no-camera boot state.
// Graph nodes, compute lanes, pulses, rings, and panel edges are expressed as
// composable SDF primitives, then shaded through a tiny material layer. The same
// pattern can grow into editor widgets, graph canvases, debug overlays, and
// compute-generated field textures without changing the caller-side GPU path.

struct IntroPush {
    float viewport_width;
    float viewport_height;
    float time_seconds;
    float intensity;
};

[[vk::push_constant]] ConstantBuffer<IntroPush> g_intro;

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VSOut vs_intro(uint vid : SV_VertexID)
{
    float2 pos2 = float2(((vid & 1u) != 0u) ? 3.0f : -1.0f,
                         ((vid & 2u) != 0u) ? 3.0f : -1.0f);
    VSOut o;
    o.pos = float4(pos2, 0.0f, 1.0f);
    o.uv = pos2 * float2(0.5f, -0.5f) + 0.5f;
    return o;
}

float hash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float sd_segment(float2 p, float2 a, float2 b)
{
    const float2 pa = p - a;
    const float2 ba = b - a;
    const float h = saturate(dot(pa, ba) / max(dot(ba, ba), 0.00001f));
    return length(pa - ba * h);
}

float sd_capsule(float2 p, float2 a, float2 b, float radius)
{
    return sd_segment(p, a, b) - radius;
}

float sd_circle(float2 p, float2 c, float radius)
{
    return length(p - c) - radius;
}

float sd_ring(float2 p, float2 c, float radius, float half_width)
{
    return abs(sd_circle(p, c, radius)) - half_width;
}

float sd_round_box(float2 p, float2 c, float2 half_extent, float radius)
{
    const float2 q = abs(p - c) - half_extent + radius;
    return length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - radius;
}

float sdf_fill(float d, float aa)
{
    return 1.0f - smoothstep(-aa, aa, d);
}

float sdf_edge(float d, float width, float aa)
{
    return 1.0f - smoothstep(width, width + aa, abs(d));
}

float sdf_line(float d, float aa)
{
    return 1.0f - smoothstep(0.0f, aa, d);
}

float sdf_glow(float d, float radius)
{
    const float x = max(d, 0.0f) / max(radius, 0.00001f);
    return 1.0f / (1.0f + x * x * 14.0f);
}

float sdf_union(float a, float b)
{
    return min(a, b);
}

float2 graph_node_pos(int index, float t)
{
    if (index == 0) return float2(-0.66f, -0.29f + sin(t * 0.71f) * 0.018f);
    if (index == 1) return float2(-0.38f,  0.05f + sin(t * 0.83f + 1.0f) * 0.015f);
    if (index == 2) return float2(-0.02f, -0.13f + sin(t * 0.67f + 2.0f) * 0.017f);
    if (index == 3) return float2( 0.35f,  0.09f + sin(t * 0.91f + 3.0f) * 0.014f);
    return float2( 0.66f, -0.28f + sin(t * 0.77f + 4.0f) * 0.018f);
}

float2 graph_node_extent(int index, float pulse)
{
    if (index == 2) return float2(0.098f + pulse * 0.006f, 0.052f);
    if (index == 1 || index == 3) return float2(0.078f, 0.043f);
    return float2(0.066f, 0.039f);
}

float field_graph_nodes(float2 p, float t, float pulse)
{
    float d = 999.0f;
    d = sdf_union(d, sd_round_box(p, graph_node_pos(0, t), graph_node_extent(0, pulse), 0.016f));
    d = sdf_union(d, sd_round_box(p, graph_node_pos(1, t), graph_node_extent(1, pulse), 0.018f));
    d = sdf_union(d, sd_round_box(p, graph_node_pos(2, t), graph_node_extent(2, pulse), 0.020f));
    d = sdf_union(d, sd_round_box(p, graph_node_pos(3, t), graph_node_extent(3, pulse), 0.018f));
    d = sdf_union(d, sd_round_box(p, graph_node_pos(4, t), graph_node_extent(4, pulse), 0.016f));
    return d;
}

float field_graph_edges(float2 p, float t)
{
    const float2 n0 = graph_node_pos(0, t);
    const float2 n1 = graph_node_pos(1, t);
    const float2 n2 = graph_node_pos(2, t);
    const float2 n3 = graph_node_pos(3, t);
    const float2 n4 = graph_node_pos(4, t);

    float d = 999.0f;
    d = sdf_union(d, sd_capsule(p, n0, n1, 0.0049f));
    d = sdf_union(d, sd_capsule(p, n1, n2, 0.0052f));
    d = sdf_union(d, sd_capsule(p, n2, n3, 0.0052f));
    d = sdf_union(d, sd_capsule(p, n2, n4, 0.0043f));
    d = sdf_union(d, sd_capsule(p, n0, n2, 0.0032f));
    return d;
}

float field_graph_packets(float2 p, float t)
{
    const float2 n0 = graph_node_pos(0, t);
    const float2 n1 = graph_node_pos(1, t);
    const float2 n2 = graph_node_pos(2, t);
    const float2 n3 = graph_node_pos(3, t);
    const float2 n4 = graph_node_pos(4, t);

    float d = 999.0f;
    d = sdf_union(d, sd_circle(p, lerp(n0, n1, frac(t * 0.23f)), 0.018f));
    d = sdf_union(d, sd_circle(p, lerp(n1, n2, frac(t * 0.31f + 0.33f)), 0.017f));
    d = sdf_union(d, sd_circle(p, lerp(n2, n3, frac(t * 0.27f + 0.62f)), 0.019f));
    d = sdf_union(d, sd_circle(p, lerp(n2, n4, frac(t * 0.19f + 0.15f)), 0.016f));
    return d;
}

float field_holo_rings(float2 p, float t, float pulse)
{
    float d = 999.0f;
    d = sdf_union(d, sd_ring(p, float2(0.0f, -0.02f), 0.19f + pulse * 0.015f, 0.0022f));
    d = sdf_union(d, sd_ring(p, float2(0.0f, -0.02f), 0.32f - pulse * 0.018f, 0.0019f));
    d = sdf_union(d, sd_ring(p, float2(0.0f, -0.02f), 0.48f + sin(t * 1.1f) * 0.010f, 0.0016f));
    return d;
}

float field_cache_fabric(float2 uv, float aspect, float t)
{
    float2 grid = uv * float2(72.0f * aspect, 40.0f);
    const float2 cell = frac(grid);
    const float2 id = floor(grid);
    const float2 local = cell - 0.5f;
    const float lane_d = min(abs(local.x), abs(local.y) * 1.85f) - 0.018f;
    const float sparse = step(0.76f, hash21(id * 0.37f + 5.0f));
    return lane_d + (1.0f - sparse) * 3.0f;
}

float field_cache_packets(float2 uv, float aspect, float t)
{
    float2 grid = uv * float2(72.0f * aspect, 40.0f);
    const float2 cell = frac(grid);
    const float2 id = floor(grid);
    const float packet_phase = frac(t * (0.45f + hash21(id) * 0.55f) + hash21(id + 19.7f));
    const float2 center = float2(packet_phase, 0.5f);
    const float sparse = step(0.83f, hash21(id * 0.37f + 5.0f));
    return sd_round_box(cell, center, float2(0.060f, 0.020f), 0.010f) +
           (1.0f - sparse) * 3.0f;
}

float field_architecture_shell(float2 p, float t, float pulse)
{
    const float drift = sin(t * 0.53f) * 0.010f;
    float d = 999.0f;
    d = sdf_union(d, sd_round_box(p, float2(0.0f, -0.02f + drift),
                                  float2(0.510f + pulse * 0.018f, 0.255f), 0.062f));
    d = sdf_union(d, sd_round_box(p, float2(-0.52f, -0.04f - drift),
                                  float2(0.245f, 0.155f), 0.044f));
    d = sdf_union(d, sd_round_box(p, float2(0.52f, -0.04f + drift),
                                  float2(0.245f, 0.155f), 0.044f));
    d = sdf_union(d, sd_round_box(p, float2(-0.27f, 0.215f + drift),
                                  float2(0.205f, 0.082f), 0.030f));
    d = sdf_union(d, sd_round_box(p, float2(0.27f, 0.215f - drift),
                                  float2(0.205f, 0.082f), 0.030f));
    d = sdf_union(d, sd_round_box(p, float2(0.0f, -0.305f - drift),
                                  float2(0.710f, 0.055f), 0.032f));
    return d;
}

float field_architecture_void(float2 p, float t)
{
    const float drift = sin(t * 0.61f) * 0.008f;
    float d = 999.0f;
    d = sdf_union(d, sd_round_box(p, float2(0.0f, -0.02f + drift),
                                  float2(0.310f, 0.125f), 0.038f));
    d = sdf_union(d, sd_round_box(p, float2(-0.52f, -0.04f),
                                  float2(0.125f, 0.064f), 0.022f));
    d = sdf_union(d, sd_round_box(p, float2(0.52f, -0.04f),
                                  float2(0.125f, 0.064f), 0.022f));
    return d;
}

float field_core_glyph(float2 p, float t)
{
    const float2 c = float2(0.0f, -0.02f);
    const float wobble = sin(t * 1.4f) * 0.006f;
    float d = 999.0f;
    d = sdf_union(d, sd_capsule(p, c + float2(0.0f, -0.100f),
                                c + float2(0.0f, 0.105f), 0.014f));
    d = sdf_union(d, sd_capsule(p, c + float2(-0.128f, 0.055f + wobble),
                                c + float2(0.128f, 0.055f - wobble), 0.013f));
    d = sdf_union(d, sd_capsule(p, c + float2(-0.088f, -0.052f - wobble),
                                c + float2(0.088f, -0.052f + wobble), 0.012f));
    d = sdf_union(d, sd_ring(p, c, 0.166f + sin(t * 0.9f) * 0.006f, 0.0060f));
    return d;
}

float field_module_tiles(float2 uv, float aspect, float t)
{
    float2 grid = uv * float2(18.0f * aspect, 10.0f);
    const float2 cell = frac(grid);
    const float2 id = floor(grid);
    const float phase = hash21(id + floor(t * 0.65f));
    const float enabled = step(0.57f, hash21(id * 1.9f + 2.0f));
    const float2 center = 0.5f + float2(sin(t * 0.45f + phase * 6.28f),
                                        cos(t * 0.38f + phase * 5.17f)) * 0.025f;
    return sd_round_box(cell, center, float2(0.300f, 0.210f), 0.065f) +
           (1.0f - enabled) * 3.0f;
}

float contour_mask(float d, float t)
{
    const float near_field = 1.0f - smoothstep(0.0f, 0.33f, abs(d));
    const float bands = 0.5f + 0.5f * sin(d * 92.0f - t * 2.6f);
    return near_field * smoothstep(0.82f, 1.0f, bands);
}

float carved_architecture_field(float2 p, float t, float pulse)
{
    const float shell_d = field_architecture_shell(p, t, pulse);
    const float void_d = field_architecture_void(p, t);
    return max(shell_d, -void_d);
}

float2 carved_architecture_normal(float2 p, float t, float pulse)
{
    const float e = 0.0025f;
    const float dx = carved_architecture_field(p + float2(e, 0.0f), t, pulse) -
                     carved_architecture_field(p - float2(e, 0.0f), t, pulse);
    const float dy = carved_architecture_field(p + float2(0.0f, e), t, pulse) -
                     carved_architecture_field(p - float2(0.0f, e), t, pulse);
    return normalize(float2(dx, dy) + float2(0.0001f, 0.0002f));
}

float3 rotate_x(float3 p, float a)
{
    const float s = sin(a);
    const float c = cos(a);
    return float3(p.x, p.y * c - p.z * s, p.y * s + p.z * c);
}

float3 rotate_y(float3 p, float a)
{
    const float s = sin(a);
    const float c = cos(a);
    return float3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
}

float3 rotate_z(float3 p, float a)
{
    const float s = sin(a);
    const float c = cos(a);
    return float3(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
}

float sd_round_box3(float3 p, float3 half_extent, float radius)
{
    const float3 q = abs(p) - half_extent + radius;
    return length(max(q, 0.0f)) + min(max(max(q.x, q.y), q.z), 0.0f) - radius;
}

float sd_sphere3(float3 p, float radius)
{
    return length(p) - radius;
}

float sd_octahedron3(float3 p, float radius)
{
    p = abs(p);
    return (p.x + p.y + p.z - radius) * 0.57735027f;
}

float sd_pyramid_like3(float3 p)
{
    const float y = p.y + 0.285f;
    const float cap = max(abs(p.x), abs(p.z)) - (0.335f - y * 0.52f);
    const float floor_d = -p.y - 0.285f;
    const float top_d = p.y - 0.315f;
    return max(max(cap, floor_d), top_d);
}

float morph_cycle_sdf(float3 p, float t)
{
    p = rotate_y(p, t * 0.72f);
    p = rotate_x(p, sin(t * 0.37f) * 0.48f + 0.28f);
    p = rotate_z(p, sin(t * 0.23f) * 0.32f);

    const float cube_d = sd_round_box3(p, float3(0.265f, 0.265f, 0.265f), 0.062f);
    const float pyramid_d = sd_pyramid_like3(p);
    const float diamond_d = sd_octahedron3(p, 0.510f);
    const float sphere_d = sd_sphere3(p, 0.305f);
    const float sharp_d = lerp(pyramid_d, diamond_d, 0.42f + 0.28f * sin(t * 0.63f));

    const float phase = frac(t * 0.075f) * 3.0f;
    if (phase < 1.0f) {
        const float m = smoothstep(0.0f, 1.0f, phase);
        return lerp(cube_d, sharp_d, m);
    }
    if (phase < 2.0f) {
        const float m = smoothstep(0.0f, 1.0f, phase - 1.0f);
        return lerp(sharp_d, sphere_d, m);
    }
    const float m = smoothstep(0.0f, 1.0f, phase - 2.0f);
    return lerp(sphere_d, cube_d, m);
}

float3 morph_cycle_normal(float3 p, float t)
{
    const float e = 0.0035f;
    const float dx = morph_cycle_sdf(p + float3(e, 0.0f, 0.0f), t) -
                     morph_cycle_sdf(p - float3(e, 0.0f, 0.0f), t);
    const float dy = morph_cycle_sdf(p + float3(0.0f, e, 0.0f), t) -
                     morph_cycle_sdf(p - float3(0.0f, e, 0.0f), t);
    const float dz = morph_cycle_sdf(p + float3(0.0f, 0.0f, e), t) -
                     morph_cycle_sdf(p - float3(0.0f, 0.0f, e), t);
    return normalize(float3(dx, dy, dz) + float3(0.0001f, 0.0002f, 0.0003f));
}

float4 render_morphing_core(float2 uv, float aspect, float t)
{
    float2 ndc = (uv - 0.5f) * float2(aspect, 1.0f);
    ndc.y += 0.035f;
    const float3 ro = float3(0.0f, 0.0f, 2.35f);
    const float3 rd = normalize(float3(ndc * 0.92f, -1.62f));

    float travel = 0.0f;
    float hit = 0.0f;
    float3 hit_p = float3(0.0f, 0.0f, 0.0f);
    float glow = 0.0f;

    [loop]
    for (int step_idx = 0; step_idx < 54; ++step_idx) {
        const float3 p3 = ro + rd * travel;
        const float d = morph_cycle_sdf(p3, t);
        glow += exp(-abs(d) * 18.0f) * 0.010f;
        if (d < 0.0025f) {
            hit = 1.0f;
            hit_p = p3;
            break;
        }
        travel += max(d * 0.74f, 0.006f);
        if (travel > 4.2f) {
            break;
        }
    }

    const float aura = saturate(glow);
    if (hit < 0.5f) {
        return float4(float3(0.035f, 0.180f, 0.230f) * aura, aura * 0.42f);
    }

    const float3 n = morph_cycle_normal(hit_p, t);
    const float3 light_dir = normalize(float3(-0.45f, 0.70f, 0.55f));
    const float3 rim_dir = normalize(float3(0.30f, -0.20f, 1.0f));
    const float diffuse = saturate(dot(n, light_dir));
    const float rim = pow(saturate(1.0f - dot(n, -rd)), 2.5f);
    const float facing = saturate(dot(n, rim_dir) * 0.5f + 0.5f);
    const float fresnel = pow(saturate(1.0f - dot(n, -rd)), 4.0f);

    const float phase = frac(t * 0.075f) * 3.0f;
    const float3 cube_col = float3(0.180f, 0.960f, 0.600f);
    const float3 pyramid_col = float3(0.170f, 0.520f, 1.000f);
    const float3 sphere_col = float3(0.680f, 0.960f, 1.000f);
    float3 base_col = cube_col;
    if (phase < 1.0f) {
        base_col = lerp(cube_col, pyramid_col, smoothstep(0.0f, 1.0f, phase));
    } else if (phase < 2.0f) {
        base_col = lerp(pyramid_col, sphere_col, smoothstep(0.0f, 1.0f, phase - 1.0f));
    } else {
        base_col = lerp(sphere_col, cube_col, smoothstep(0.0f, 1.0f, phase - 2.0f));
    }

    float3 col = base_col * (0.18f + diffuse * 0.72f + facing * 0.18f);
    col += float3(0.55f, 0.92f, 1.00f) * rim * 0.55f;
    col += float3(0.80f, 1.00f, 0.85f) * fresnel * 0.34f;
    col += float3(0.10f, 0.40f, 0.55f) * aura;
    const float alpha = saturate(0.34f + rim * 0.16f + fresnel * 0.12f);
    return float4(col, alpha);
}

[shader("fragment")]
float4 fs_intro(VSOut i) : SV_Target
{
    const float2 viewport = max(float2(g_intro.viewport_width, g_intro.viewport_height),
                                float2(1.0f, 1.0f));
    const float aspect = viewport.x / viewport.y;
    const float t = g_intro.time_seconds;
    const float intensity = saturate(g_intro.intensity);
    const float aa = 1.65f / viewport.y;

    const float2 uv = i.uv;
    const float2 p = (uv - 0.5f) * float2(aspect, 1.0f);
    const float pulse = 0.5f + 0.5f * sin(t * 2.3f);

    const float2 radial = (uv - 0.5f) * float2(aspect, 1.0f);
    float3 col = lerp(float3(0.004f, 0.010f, 0.020f),
                      float3(0.020f, 0.035f, 0.056f),
                      saturate(1.0f - length(radial) * 0.74f));

    const float tile_d = field_module_tiles(uv, aspect, t);
    col += sdf_fill(tile_d, aa * 1.5f) * float3(0.004f, 0.020f, 0.034f);
    col += sdf_edge(tile_d, 0.010f, aa * 1.4f) * float3(0.006f, 0.050f, 0.072f);

    const float fabric_d = field_cache_fabric(uv, aspect, t);
    const float packet_d = field_cache_packets(uv, aspect, t);
    col += sdf_line(fabric_d, aa * 1.10f) * float3(0.003f, 0.018f, 0.034f);
    col += sdf_line(packet_d, aa * 1.2f) * float3(0.020f, 0.090f, 0.150f);
    col += sdf_glow(packet_d, 0.025f) * float3(0.002f, 0.015f, 0.032f);

    const float shell_d = field_architecture_shell(p, t, pulse);
    const float void_d = field_architecture_void(p, t);
    const float carved_shell_d = carved_architecture_field(p, t, pulse);
    const float shell_fill = sdf_fill(carved_shell_d, aa * 2.0f);
    const float shell_depth = saturate(-carved_shell_d / 0.150f);
    const float2 shell_n = carved_architecture_normal(p, t, pulse);
    const float shell_light = saturate(dot(shell_n, normalize(float2(-0.55f, -0.82f))) * 0.5f + 0.5f);
    col += sdf_glow(shell_d, 0.360f) * float3(0.016f, 0.090f, 0.095f);
    col += shell_fill * lerp(float3(0.020f, 0.125f, 0.125f),
                             float3(0.080f, 0.420f, 0.340f),
                             shell_depth * 0.62f + shell_light * 0.38f);
    col += sdf_fill(void_d, aa * 2.0f) * float3(0.000f, 0.006f, 0.012f);
    col += sdf_edge(shell_d, 0.0100f, aa * 1.7f) * float3(0.210f, 1.000f, 0.630f);
    col += sdf_edge(void_d, 0.0080f, aa * 1.6f) * float3(0.140f, 0.600f, 1.000f);
    col += contour_mask(shell_d, t) * float3(0.040f, 0.280f, 0.270f);
    col += contour_mask(void_d, t + 1.2f) * float3(0.050f, 0.210f, 0.380f);

    const float4 morph_core = render_morphing_core(uv, aspect, t);
    col = lerp(col, col + morph_core.rgb, morph_core.a);

    const float glyph_d = field_core_glyph(p, t);
    col += sdf_line(glyph_d, aa * 2.2f) * float3(0.430f, 0.930f, 0.760f);
    col += sdf_glow(glyph_d, 0.070f) * float3(0.055f, 0.210f, 0.160f);

    const float packet_graph_d = field_graph_packets(p, t);
    const float ring_d = field_holo_rings(p, t, pulse);

    col += sdf_line(packet_graph_d, aa * 1.8f) * float3(0.090f, 0.280f, 0.360f);
    col += sdf_glow(packet_graph_d, 0.035f) * float3(0.008f, 0.050f, 0.070f);

    col += sdf_line(ring_d, aa * 2.1f) * float3(0.060f, 0.255f, 0.480f);
    col += sdf_glow(ring_d, 0.030f) * float3(0.010f, 0.050f, 0.095f);

    const float panel_d = sd_round_box(p, float2(0.0f, -0.02f), float2(0.88f, 0.42f), 0.055f);
    col += sdf_edge(panel_d, 0.0022f, aa * 2.0f) * float3(0.045f, 0.190f, 0.320f);

    const float diagonal =
        smoothstep(0.975f, 1.0f, sin((uv.x * 18.0f - uv.y * 12.0f + t * 1.8f)) * 0.5f + 0.5f);
    col += diagonal * float3(0.020f, 0.120f, 0.240f);

    const float scan = 0.5f + 0.5f * sin((uv.y * viewport.y) * 0.052f + t * 8.0f);
    col += scan * float3(0.003f, 0.010f, 0.018f);

    const float vignette = smoothstep(0.92f, 0.18f, length((uv - 0.5f) * float2(aspect, 1.0f)));
    col *= 0.35f + 0.85f * vignette;

    return float4(saturate(col * intensity), 1.0f);
}
