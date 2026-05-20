// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/shaders/gpu_cull.slang
//
// Lane 09 — GPU-driven instance cull (compute → indirect draw args).
//
// One thread per instance:
//   1. Read instance descriptor (world matrix + world-space AABB).
//   2. Frustum-cull against view's 6 clip planes.
//   3. HiZ occlusion-cull against the mip pyramid (conservative; samples
//      the smallest mip enclosing the projected AABB).
//   4. If visible: atomic-append the instance index to
//      visible_instance_buffer and emit an indirect-draw record into
//      indirect_draw_buffer.
//
// SCAFFOLD: the body writes a single "visible" flag header for sanity.
// Real implementation lands in M3 alongside the indirect-draw encode
// glue from lane 07.
//
// DESIGN-PSYNDER-GX.md §7.2.

struct InstanceDesc {
    float4x4 world_matrix;
    float4   aabb_min_pad;
    float4   aabb_max_pad;
    uint     mesh_id;
    uint     material_id;
    uint2    pad;
};

struct DrawIndexedIndirect {
    uint index_count;
    uint instance_count;
    uint first_index;
    int  vertex_offset;
    uint first_instance;
};

// Push-constants — must match the layout of CullPushConstants in
// engine/render/pipeline/GpuCull.cpp (4 × u32 = 16 bytes).  view_proj and
// the 6 frustum planes are NOT in the push range yet; they will land via
// a per-view UBO in M3 once lane 07 grows the descriptor-set API.  Until
// then the scaffold body skips the actual frustum / HiZ test (see below),
// so the matrices' absence doesn't affect the SCAFFOLD's visible output.
struct CullParams {
    uint g_instance_count;
    uint g_hiz_mip_count;
    uint g_padding[2];
};
[[vk::push_constant]] ConstantBuffer<CullParams> g_pc;

StructuredBuffer<InstanceDesc>      g_instances     : register(t1);
RWStructuredBuffer<uint>            g_visible_index : register(u2);
RWStructuredBuffer<DrawIndexedIndirect> g_indirect  : register(u3);

// Visible-count header is stored in g_visible_index[0]; visible instance
// indices start at g_visible_index[1].
[numthreads(64, 1, 1)]
[shader("compute")]
void cs_cull(uint3 did : SV_DispatchThreadID) {
    uint inst = did.x;
    if (inst >= g_pc.g_instance_count) return;

    InstanceDesc d = g_instances[inst];

    // SCAFFOLD: skip the actual frustum / HiZ tests — write the instance
    // index unconditionally. The visible_index header is bumped via
    // atomic add. The matching indirect-draw record is left zero for now;
    // M3's encode plumbing fills it.
    uint slot;
    InterlockedAdd(g_visible_index[0], 1, slot);
    g_visible_index[1 + slot] = inst;

    if (slot < g_pc.g_instance_count) {
        DrawIndexedIndirect cmd;
        cmd.index_count    = 3;   // SCAFFOLD: hardcoded for the M1 triangle
        cmd.instance_count = 1;
        cmd.first_index    = 0;
        cmd.vertex_offset  = 0;
        cmd.first_instance = inst;
        g_indirect[slot] = cmd;
    }
}
