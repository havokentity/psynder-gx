// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/upscale/metalfx.cpp - MetalFX upscaler stub (lane 11).
//
// M2 selection: MetalFX is the *named* upscaler that lane 11 is wiring up
// end-to-end as the demonstration path (no third-party SDK dependency on
// Apple Silicon -- MTLFXSpatialScaler / MTLFXTemporalScaler are in the
// stock Metal framework on macOS 13+).
//
// Why this file is .cpp rather than .mm:
//   The lane 11 leaf directory must compile cleanly on Win/Linux as a
//   stub. Renaming this TU to metalfx.mm would require lane 25 to add
//   the Metal + MetalFX frameworks to the link line for psynder_render_post
//   on macOS, which is orchestrator-owned territory (engine/render/CMakeLists.txt
//   is shared). The real [scaler encodeToCommandBuffer:] hookup is filed
//   as a lane-11 follow-up issue in INTEGRATION.txt; this TU stays a
//   safe, link-clean cpp stub until then.
//
// Behaviour today:
//   - is_supported() returns true on macOS 13+ (we trust the deployment
//     target check; runtime introspection lands with the .mm rewrite).
//   - is_supported() returns false on other platforms.
//   - evaluate() is a no-op: PostProcess.cpp passes the pre-tonemap HDR
//     scene through to the tonemap pass at native resolution, which is the
//     correct "no-upscale" behaviour.

#include "metalfx.h"

namespace psynder::render::post::upscale {

MetalFx::MetalFx(gpu::Device* device) : device_(device)
{
    // M3+ macOS: build a MTLFXTemporalScalerDescriptor here, configure
    // input/output pixel formats and color spaces from PostDesc, call
    // newTemporalScalerWithDescriptor: against the MTLDevice fetched
    // through PublicGpuInternal. Detect Apple Silicon unified memory
    // for the quality budget heuristic.
    (void)device_;
}

MetalFx::~MetalFx()
{
    // M3+: id<MTLFXTemporalScaler> release happens via ARC in the .mm
    // rewrite. Nothing to do here in the .cpp stub.
}

bool MetalFx::is_supported() const noexcept
{
    // While evaluate() remains a documented no-op, is_supported() MUST
    // return false on every platform — otherwise PostProcess::run_frame
    // would route the upscale request through a stub that writes nothing
    // to `tonemap_out`, leaving the destination untouched and silently
    // breaking the upscale path.  Copilot PR #17 review caught the gap.
    //
    // The .mm rewrite (M3+) will replace this with a real
    // `[MTLFXTemporalScalerDescriptor supportsDevice:mtl_device_]` check
    // and ALSO narrow the platform guard to `__APPLE__ && TARGET_OS_OSX`
    // so iOS / tvOS / visionOS hosts don't accidentally enter the
    // macOS-13+-only API path.
    return false;
}

void MetalFx::evaluate(const UpscaleInput& /*input*/)
{
    // M3+ (macOS): copy UpscaleInput into MTLFXTemporalScalerDescriptor,
    // bind input/output/depth/motion textures via the active
    // psy::gpu::CmdBuffer's Metal command buffer, call
    // [scaler encodeToCommandBuffer:]. Today: no-op (PostProcess.cpp's
    // downstream tonemap reads the un-upscaled HDR directly).
}

} // namespace psynder::render::post::upscale
