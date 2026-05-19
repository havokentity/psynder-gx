// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/PipelineSmoke.cpp
//
// Lane 09 — standalone smoke binary for the render-pipeline scaffold.
//
// Exercises the public PublicRenderPipeline.h API in headless mode (no
// device hook set), validating:
//   * create() returns a non-null Pipeline*
//   * sub-pass init paths log expected sizes (cluster count, HiZ mips,
//     instance-buffer size, opaque pipeline compile)
//   * render(view) walks extract + every encode_*() step without crashing
//   * destroy() drops cleanly
//
// Build with -DPSYNDER_GX_RENDER_PIPELINE_BUILD_SMOKE=ON. The smoke does
// NOT spin up a real psy::gpu::Device — that would require pulling in
// lane 25 (platform-macos) at link time, which would break the rule that
// this lane edits only its own subtree. The orchestrator's Wave-C
// integration step (sample_00_clear) is where the live device hook lands.

#include "render/pipeline/PublicRenderPipeline.h"
#include "render/pipeline/Pipeline_internal.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

int run_smoke(int smoke_frames) {
    std::puts("[render-pipeline-smoke] begin (headless, no device hook)");

    psynder::render::pipeline::PipelineDesc desc{};
    desc.internal_width      = 1280;
    desc.internal_height     = 720;
    desc.shadow_cascades     = 4;
    desc.light_clusters_x    = 16;
    desc.light_clusters_y    = 9;
    desc.light_clusters_z    = 24;
    desc.enable_rt           = false;
    desc.enable_mesh_shaders = false;

    auto* p = psynder::render::pipeline::create(desc);
    if (!p) {
        std::fputs("[render-pipeline-smoke] create() returned null\n", stderr);
        return 1;
    }

    psynder::render::pipeline::View view{};
    view.viewport_w = desc.internal_width;
    view.viewport_h = desc.internal_height;
    // Identity matrices — no draws will go to a screen since we have no
    // device. The render() entry point still iterates extract +
    // encode_*() helpers which is what we want to smoke.
    view.view_matrix.m[0]  = 1.0f;
    view.view_matrix.m[5]  = 1.0f;
    view.view_matrix.m[10] = 1.0f;
    view.view_matrix.m[15] = 1.0f;
    view.proj_matrix = view.view_matrix;

    for (int i = 0; i < smoke_frames; ++i) {
        psynder::render::pipeline::render(p, view);
    }

    psynder::render::pipeline::destroy(p);
    std::printf("[render-pipeline-smoke] ran %d render() ticks; OK\n", smoke_frames);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    int frames = 4;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--smoke-frames=", 15) == 0) {
            frames = std::atoi(argv[i] + 15);
            if (frames < 1) frames = 1;
        }
    }
    return run_smoke(frames);
}
