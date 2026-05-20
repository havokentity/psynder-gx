// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/shaders/light_cluster_build.slang
//
// Lane 09 — forward+ light cluster build (compute pass).
//
// Per cluster: test all scene lights against the cluster's view-space
// AABB; emit the matching light indices into cluster_light_buffer.
//
// Dispatch shape (decided by Pipeline.cpp): one workgroup per cluster.
// Each thread in the group tests a chunk of the lights table.
//
// SCAFFOLD: real impl needs the per-cluster AABB build (separate compute
// dispatch, runs once per camera-projection change) + an atomic offset
// allocator inside cluster_light_buffer. For the scaffold this entry
// point compiles and writes zero counts.
//
// See DESIGN-PSYNDER-GX.md §7.3.

struct Light {
    float3 position_ws;
    float  range;
    float3 color;
    float  pad;
};

struct ClusterAabb {
    float4 min_xyz_pad;
    float4 max_xyz_pad;
};

// Push-constants — must match the layout of ClusterPushConstants in
// engine/render/pipeline/LightCluster.cpp (4 × u32 = 16 bytes).  The view
// and projection matrices are NOT in the push range; they will land via
// a per-view UBO in M3 once lane 07 grows the descriptor-set API.  Until
// then the scaffold only needs the cluster grid dimensions to validate
// the workgroup count.
struct ClusterParams {
    uint g_cluster_x;
    uint g_cluster_y;
    uint g_cluster_z;
    uint g_light_count;
};
[[vk::push_constant]] ConstantBuffer<ClusterParams> g_pc;

StructuredBuffer<Light>       g_lights        : register(t1);
StructuredBuffer<ClusterAabb> g_cluster_aabbs : register(t2);
RWStructuredBuffer<uint2>     g_cluster_offset_count : register(u3);
RWStructuredBuffer<uint>      g_cluster_light_index  : register(u4);

[numthreads(64, 1, 1)]
[shader("compute")]
void cs_build(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    // One group per cluster (scaffold layout).
    uint cluster_id = gid.x;
    uint total_clusters = g_pc.g_cluster_x * g_pc.g_cluster_y * g_pc.g_cluster_z;
    if (cluster_id >= total_clusters) return;

    // SCAFFOLD: write a zero header so the fragment shader iterates no
    // cluster-lights yet. The real impl will:
    //   1. Read g_cluster_aabbs[cluster_id]
    //   2. Loop over g_lights (stride = 64 per thread)
    //   3. Test sphere-vs-AABB
    //   4. Atomic-append to g_cluster_light_index, bump local count
    //   5. Thread 0 writes (offset, count) into g_cluster_offset_count
    if (tid.x == 0) {
        g_cluster_offset_count[cluster_id] = uint2(0, 0);
    }
}
