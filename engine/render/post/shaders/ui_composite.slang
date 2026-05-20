// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/shaders/ui_composite.slang
//
// Lane 11 - Final UI compositing pass (DESIGN section 10.6).
//
// Composites the lane 21 UI layer onto the tonemapped scene with STRAIGHT
// (non-premultiplied) alpha blending, per the task spec:
//
//   out.rgb = scene.rgb * (1 - ui.a) + ui.rgb * ui.a
//   out.a   = 1
//
// If the UI texture is absent (has_ui == 0), the pass writes the scene
// unchanged -- this is the Wave-A path where lane 21 hasn't bound a UI
// atlas yet. The post host (PostProcess.cpp) sets `has_ui` accordingly.
//
// Two atlas slots are reserved (RmlUi player HUD/menu and immediate-mode
// debug overlay). Wave-A binds at most one; the second slot is treated
// the same way as the first when its has_*_ui flag is set.
//
// Integration notes for lane 21 (ui):
//   - Atlases are RGBA8 (sRGB or linear-rgba; the shader treats the
//     channels as-is). Updated each frame by lane 21 before run_frame().
//   - Pass nullptr / has_*_ui = 0 for an inactive atlas.
//   - For pixel-perfect UI, atlases should match output_width *
//     output_height in PostDesc (post-upscale resolution).
//
// Slang compiles to SPIR-V (Vulkan) and Metal IR (macOS) via psy::shader.

// ─── Textures ────────────────────────────────────────────────────────────
Texture2D<float4>   t_scene;     // tonemapped scene (sRGB-encoded by HW on store)
Texture2D<float4>   t_ui_rml;    // RmlUi atlas (straight alpha, may be unused)
Texture2D<float4>   t_ui_imm;    // immediate-mode atlas (straight alpha)
SamplerState        s_linear;    // bilinear clamp

// ─── Params ──────────────────────────────────────────────────────────────
struct UICompositeParams
{
    uint  has_ui_rml;   // 1 if t_ui_rml has real content
    uint  has_ui_imm;   // 1 if t_ui_imm has real content
    float _pad0;
    float _pad1;
};

ParameterBlock<UICompositeParams> g_ui;

// ─── Helpers ─────────────────────────────────────────────────────────────

// Straight (non-premultiplied) alpha-over: classic Porter-Duff "over"
// with the convention that ui.rgb is NOT scaled by ui.a.
//
//   out.rgb = dst.rgb * (1 - src.a) + src.rgb * src.a
//
// (Premultiplied alpha would skip the src.rgb * src.a term and assume the
// texture already baked it in. The task spec asks for straight alpha.)
float4 alpha_over_straight(float4 dst, float4 src)
{
    float3 rgb = dst.rgb * (1.0f - src.a) + src.rgb * src.a;
    return float4(rgb, 1.0f);
}

// ─── Vertex shader (full-screen triangle) ────────────────────────────────
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VSOut vs_ui_composite(uint vid : SV_VertexID)
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
float4 fs_ui_composite(VSOut i) : SV_Target
{
    float4 result = t_scene.Sample(s_linear, i.uv);

    if (g_ui.has_ui_rml != 0u)
    {
        float4 rml = t_ui_rml.Sample(s_linear, i.uv);
        result = alpha_over_straight(result, rml);
    }

    if (g_ui.has_ui_imm != 0u)
    {
        float4 imm = t_ui_imm.Sample(s_linear, i.uv);
        result = alpha_over_straight(result, imm);
    }

    return result;
}
