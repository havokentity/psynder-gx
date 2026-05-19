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
//                       2. light cluster build (compute dispatch)
//                       3. depth pre-pass (render pass) + HiZ pyramid
//                          (compute dispatches per mip)
//                       4. GPU cull (compute dispatch → indirect args)
//                       5. opaque forward+ pass (draw indexed)
//                       The compute passes share one cmd buffer; the
//                       depth pre-pass + opaque pass each open their own
//                       so each begin_render scope is clean (Metal can't
//                       nest compute inside a render encoder — backend
//                       returns "dispatch called inside a render pass;
//                       ignored" otherwise).
//   destroy(p)        — drop resources via Handle<T> destructors + delete
//                       the Pipeline struct.
//
// See DESIGN-PSYNDER-GX.md §7.1–§7.3.
//
// Wave-C update (lane 07 cmd encoder API + lane 08 PSO registration
// landed): the per-pass encode_*() helpers now issue real begin_render /
// bind_pipeline / dispatch / draw_indexed calls against the public
// PublicGpu.h surface. The previous diagnostic-only fallthrough only
// kicks in if the device hook is null (headless tests).

#include "render/pipeline/PublicRenderPipeline.h"
#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"
#include "jobs/JobSystem.h"

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

const Pipeline::PassStats& pipeline_stats(const Pipeline* p) {
    static const Pipeline::PassStats kEmpty{};
    return p ? p->stats : kEmpty;
}

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
//
// Per-pass cmd-buffer layout (DESIGN §7.1 ordering):
//
//   cmd_compute  : light_cluster_build (dispatch) +
//                  gpu_cull             (dispatch)
//                  (encoded together — both are pure compute and can share
//                   a single Metal compute scope; the backend opens / closes
//                   the compute encoder around each dispatch internally)
//   cmd_depth    : depth pre-pass render pass (draw_indexed across visible
//                  instances) + HiZ downsample compute chain (one dispatch
//                  per mip level)
//   cmd_opaque   : opaque forward+ render pass (draw_indexed across
//                  visible instances; reads depth_texture with depth-equal
//                  compare)
//
// The orchestrator brief calls out the JobSystem usage: we submit the
// CPU-side extract as a job (one per archetype chunk) before starting
// the GPU recording. The recording itself happens on the calling thread
// because the M0 single-frame backend does not yet expose per-worker
// cmd-buffer allocation — that's a Lane 07 M3 expansion. When that lands,
// the three cmd_* recordings below trivially parallelise via additional
// JobSystem::submit calls (each closure already gets its own CmdBuffer*).
void render(Pipeline* p, const View& view) {
    if (!p) return;
    ++p->frame_index;

    // 1. CPU extract — dispatch as a job so the JobSystem accounts for it.
    //    In headless / synchronous Phase-0 the job runs inline; once Lane 04
    //    ships the work-stealing scheduler this becomes a true parallel
    //    walk over archetype chunks (DESIGN §2.4 + §3).
    struct ExtractCtx { Pipeline* p; const View* view; };
    ExtractCtx ectx{p, &view};
    psynder::jobs::JobDesc edesc{};
    edesc.name = "render-pipeline.extract";
    edesc.user = &ectx;
    edesc.fn   = [](void* user) noexcept {
        auto* ctx = static_cast<ExtractCtx*>(user);
        extract_renderables(ctx->p, *ctx->view);
    };
    auto h = psynder::jobs::JobSystem::Get().submit(edesc);
    psynder::jobs::JobSystem::Get().wait(h);

    if (!p->device) {
        // Headless mode (no device hook): nothing more to do beyond the
        // extract (already populated p->renderables). The per-pass encode_*
        // helpers each short-circuit when cmd == nullptr.
        return;
    }

    // 2. Light cluster build — no HiZ dependency, runs as soon as the
    //    cluster grid is set up.  Pure compute; submitted on its own cmd
    //    buffer so the GPU can overlap it with the depth pre-pass below.
    if (gpu::CmdBuffer* cmd_cluster = gpu::cmd_open(p->device)) {
        encode_light_cluster_build(p, cmd_cluster, view);
        gpu::cmd_submit(p->device, cmd_cluster);
    }

    // 3. Depth pre-pass + HiZ downsample chain.  Depth is a render pass;
    //    HiZ is compute and MUST come after end_render so the Metal compute
    //    encoder can open cleanly (see MetalBackend.mm: "dispatch called
    //    inside a render pass; ignored").  They share one cmd buffer.
    if (gpu::CmdBuffer* cmd_depth = gpu::cmd_open(p->device)) {
        encode_depth_prepass (p, cmd_depth, view);
        encode_hiz_downsample(p, cmd_depth);
        gpu::cmd_submit(p->device, cmd_depth);
    }

    // 4. GPU instance cull.  Order matters: this consumes the HiZ pyramid
    //    populated by step 3 (occlusion test against the conservative-max
    //    mip chain).  An earlier revision ran cull before HiZ — that would
    //    have been silently incorrect once the SCAFFOLD body in gpu_cull
    //    .slang grows real HiZ sampling.  Copilot's PR #8 review caught
    //    the ordering bug; the cull now runs on its own cmd buffer
    //    submitted *after* cmd_depth so the HiZ writes are visible.
    if (gpu::CmdBuffer* cmd_cull = gpu::cmd_open(p->device)) {
        encode_gpu_cull(p, cmd_cull, view);
        gpu::cmd_submit(p->device, cmd_cull);
    }

    // 5. Opaque forward+ pass. Distinct cmd buffer for the same scope
    //    reason as above (render pass) and so the orchestrator can
    //    optionally interleave it with the post lane (lane 11) once that
    //    integration lands.
    if (gpu::CmdBuffer* cmd_opaque = gpu::cmd_open(p->device)) {
        encode_opaque(p, cmd_opaque, view);
        gpu::cmd_submit(p->device, cmd_opaque);
    }
}

} // namespace psynder::render::pipeline
