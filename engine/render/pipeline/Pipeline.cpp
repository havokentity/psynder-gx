// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/Pipeline.cpp
//
// Lane 09 — forward+ render pipeline top-level entry points.
//
// Implements the public PublicRenderPipeline.h API:
//   create(desc)      — allocate + init all sub-passes (cluster, HiZ,
//                       cull, opaque) and pre-allocate the M1 triangle
//                       resources via psy::gpu::create_buffer.
//   render(p, view)   — per-frame:
//                       1. ECS extract (M1 = hardcoded triangle; M2+ =
//                          lane 06 scene query)
//                       2. light cluster build (compute)
//                       3. depth pre-pass + HiZ pyramid (compute)
//                       4. GPU cull (compute → indirect args)
//                       5. opaque forward+ pass (draw indirect)
//                       Each step calls into the per-sub-pass encoder
//                       defined in LightCluster/Hiz/GpuCull/OpaquePass.cpp.
//   destroy(p)        — drop resources via Handle<T> destructors + delete
//                       the Pipeline struct.
//
// See DESIGN-PSYNDER-GX.md §7.1–§7.3.
//
// Cross-lane integration gap (Wave B): PublicGpu.h does not yet expose
// per-pass encoder APIs (begin_render / draw / dispatch / end_render).
// The encode_*() helpers in this lane therefore prepare resources +
// indirect args and rely on lane 07's cmd_submit which, at M0, encodes
// an animated clear color to the swapchain. INTEGRATION.txt describes
// the handoff + the Issue we file against lane 07.

#include "render/pipeline/PublicRenderPipeline.h"
#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace psynder::render::pipeline {

namespace {

// The device pointer that the application passed to psy::gpu::create_device.
// PublicRenderPipeline.h does NOT carry a device pointer through PipelineDesc
// (the contract is frozen) — we lift it from a process-wide hook that the
// sample sets up before calling create(). M2+: extend PipelineDesc through
// a coordinated cross-lane change.
gpu::Device* g_device_hook = nullptr;

} // namespace

// ─── Test hook used by samples / integration code ───────────────────────
//
// Not exposed in PublicRenderPipeline.h (the contract is frozen).
// Callers set this before pipeline::create() so the lane can locate the
// GPU device. A future cross-lane Issue (lane09-002) extends PipelineDesc.
void set_device_hook(gpu::Device* dev) { g_device_hook = dev; }

// ─── create / destroy ───────────────────────────────────────────────────
Pipeline* create(const PipelineDesc& desc) {
    auto* p = new (std::nothrow) Pipeline();
    if (!p) {
        std::fputs("[psy::render::pipeline] create: out of memory\n", stderr);
        return nullptr;
    }
    p->desc        = desc;
    p->device      = g_device_hook;
    p->frame_index = 0;

    if (!p->device) {
        // Allowed at "headless construction" time — e.g. unit tests that
        // exercise the pipeline shape without a real GPU. Sub-pass init
        // gracefully no-ops below when device is null.
        std::fputs("[psy::render::pipeline] create: no device hook set; running headless\n",
                   stderr);
    }

    // Pre-allocate the M1 hardcoded triangle so render() has something to
    // draw immediately. Failure is non-fatal — M2 may overwrite anyway.
    // In headless mode (no device) the upload deliberately no-ops; we
    // only log a real failure when the device was present.
    if (!upload_m1_triangle(p) && p->device) {
        std::fputs("[psy::render::pipeline] create: M1 triangle upload failed\n", stderr);
    }

    // Stand up the sub-pass GPU state. Each init_*() is independent;
    // any one failing is logged but doesn't abort the others — the
    // pipeline degrades gracefully (skips the missing pass at render).
    if (!init_light_cluster(p)) {
        std::fputs("[psy::render::pipeline] create: light cluster init failed\n", stderr);
    }
    if (!init_hiz_pyramid(p)) {
        std::fputs("[psy::render::pipeline] create: HiZ pyramid init failed\n", stderr);
    }
    if (!init_gpu_cull(p)) {
        std::fputs("[psy::render::pipeline] create: GPU cull init failed\n", stderr);
    }
    if (!init_opaque_pass(p)) {
        std::fputs("[psy::render::pipeline] create: opaque pass init failed\n", stderr);
    }

    std::printf("[psy::render::pipeline] created: %ux%u  clusters=%ux%ux%u  rt=%d  mesh=%d\n",
                desc.internal_width, desc.internal_height,
                desc.light_clusters_x, desc.light_clusters_y, desc.light_clusters_z,
                static_cast<int>(desc.enable_rt),
                static_cast<int>(desc.enable_mesh_shaders));
    return p;
}

void destroy(Pipeline* p) {
    if (!p) return;
    release_pipeline_resources(p);
    delete p;
}

// ─── per-frame render() ─────────────────────────────────────────────────
void render(Pipeline* p, const View& view) {
    if (!p) return;
    ++p->frame_index;

    // 1. CPU extract — collect visible renderables.
    extract_renderables(p, view);

    if (!p->device) {
        // Headless mode (no device hook): nothing more to do.
        return;
    }

    // 2-5. Open a command buffer and walk the sub-passes.
    //
    // The current cross-lane API (PublicGpu.h) does NOT expose
    // begin_render / draw / dispatch / end_render. cmd_submit on its own
    // triggers the M0 Metal-backend animated-clear encode. The encode_*()
    // helpers below therefore:
    //   - build any per-frame uniforms via buffer_map / buffer_unmap
    //     (lane 07's mid-frame helpers ARE in the contract)
    //   - log the would-be dispatch counts for diagnostics
    //   - skip the actual API encode (filed as Issue lane09-001)
    //
    // Once lane 07 extends PublicGpu.h with cmd encoder APIs, each
    // encode_*() will replace its diagnostic-only body with the real
    // bindings + dispatches.

    gpu::CmdBuffer* cmd = gpu::cmd_open(p->device);
    if (!cmd) {
        // Frame dropped — swapchain timeout etc. lane 07 already logged.
        return;
    }

    encode_light_cluster_build(p, cmd, view);
    encode_depth_prepass     (p, cmd, view);
    encode_hiz_downsample    (p, cmd);
    encode_gpu_cull          (p, cmd, view);
    encode_opaque            (p, cmd, view);

    gpu::cmd_submit(p->device, cmd);
}

} // namespace psynder::render::pipeline
