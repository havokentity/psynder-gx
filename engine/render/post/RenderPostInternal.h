// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/RenderPostInternal.h
//
// Lane 11 — INTERNAL handshake header. Declares the per-frame setters that
// lanes 09 (render-pipeline) and 21 (UI) call to feed the post-process pass
// without making the symbols part of the frozen public surface
// (engine/render/post/PublicRenderPost.h).
//
// Promoting any of these to the public header is an orchestrator-level ABI
// decision — until then this header lives inside the post lane and is
// `#include`d by cross-lane callers that have a legitimate need.
//
// Threading: these are all SINGLE-THREADED-WRITER call sites — the game
// loop wires them once per frame before `run_frame()`.  Reading the
// resulting state happens on the render thread inside `run_frame()`; the
// two are sequenced by the frame's tick, not by an internal mutex.
//
// See engine/render/post/PostProcess.cpp for the implementations.

#pragma once

namespace psynder::gpu { struct Texture; }

namespace psynder::render::post {

// Wire the linear-HDR scene-color attachment that bloom / tonemap consume.
// Pass nullptr to clear (M0 path, no scene to post-process).
void set_scene_color(psynder::gpu::Texture* tex);

// Wire the optional UI layers that `ui_composite.slang` alpha-blends on top
// of the tonemapped result.  Either pointer may be null to disable that
// layer; lane 11 tracks `has_ui_*` flags internally so the shader knows.
void set_ui_textures(psynder::gpu::Texture* ui_rml,
                     psynder::gpu::Texture* ui_imm);

// Runtime exposure override (EV stops).  PostDesc::exposure_ev sets the
// initial value at init(); this setter swaps it without re-init for
// camera auto-exposure / the `r_exposure` cvar.
void set_exposure_ev(float ev);

}  // namespace psynder::render::post
