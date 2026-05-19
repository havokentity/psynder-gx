// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ui/imm/Imm.h
//
// Lane 21 — immediate-mode UI.  In-viewport overlays ONLY (gizmos, brush
// previews, debug HUD, perf graphs).
//
// GX change from Psynder Wave-A: `begin_frame` no longer takes a CPU
// framebuffer — it accepts a GPU CmdBuffer + pipeline + pre-allocated
// HostVisible vertex/index buffers + viewport size.  Drawing calls push
// batched geometry; `end_frame` flushes the batch to the GPU.
//
// Public header is frozen per AGENTS.md §public-header-contracts.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <string_view>

namespace psynder::ui::imm {

// Call once per frame before any widget/overlay call.
// `vb` and `ib` must be pre-created HostVisible buffers sized for at least
// kImmMaxVertices × sizeof(ImmVertex) and kImmMaxIndices × sizeof(u32)
// respectively (see detail/GpuBatch.h for the constants).
void begin_frame(psynder::gpu::CmdBuffer*        cmd,
                 psynder::shader::PipelineHandle pipeline,
                 psynder::gpu::Buffer*           vb,
                 psynder::gpu::Buffer*           ib,
                 f32 viewport_width,
                 f32 viewport_height);

// Flushes the geometry batch and releases the CmdBuffer borrow.
void end_frame();

// Input state — call exactly once between begin_frame() and the first widget.
void set_input(math::Vec2 mouse_pos, bool mouse_down);

// ─── Drawing primitives ──────────────────────────────────────────────────
bool button(math::Vec2 position, math::Vec2 size, std::string_view label);
void label(math::Vec2 position, std::string_view text, u32 rgba = 0xFFFFFFFFu);
void line(math::Vec2 a, math::Vec2 b, u32 rgba);
void rect_outline(math::Vec2 origin, math::Vec2 size, u32 rgba);
void filled_rect(math::Vec2 origin, math::Vec2 size, u32 rgba);

}  // namespace psynder::ui::imm
