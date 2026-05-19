// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ui/imm/Overlay.h
//
// Lane 21 — in-viewport overlay extras beyond the frozen Imm.h surface.
//
// GX change from Psynder Wave-A: all functions that previously took a
// `render::Framebuffer&` now operate on the implicit GpuBatch that was
// opened by `imm::begin_frame()`.  The framebuffer parameter is gone.
// All functions operate on the GpuBatch opened by imm::begin_frame().

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <string_view>

namespace psynder::ui::imm {

// ─── Perf graph ──────────────────────────────────────────────────────────
void graph(math::Vec2 origin,
           math::Vec2 size,
           f32        sample_ms,
           f32        max_ms = 0.0f,
           std::string_view caption = {});

void graph_series(math::Vec2 origin,
                  math::Vec2 size,
                  std::span<const f32> samples,
                  f32 max_value = 0.0f,
                  std::string_view caption = {});

// ─── Selection highlight ─────────────────────────────────────────────────
void selection_highlight(math::Vec2 origin, math::Vec2 size);

// ─── Brush preview ───────────────────────────────────────────────────────
void brush_preview(math::Vec2 centre, f32 radius, f32 falloff_radius = 0.0f);

// ─── 3D manipulator gizmos ───────────────────────────────────────────────
enum class GizmoAxis : i8 { None = -1, X = 0, Y = 1, Z = 2 };

struct GizmoProjection {
    math::Vec2 origin{};
    math::Vec2 axis_x{};
    math::Vec2 axis_y{};
    math::Vec2 axis_z{};
    f32        arm_length = 64.0f;
};

GizmoAxis gizmo_translate(const GizmoProjection& proj);
GizmoAxis gizmo_rotate   (const GizmoProjection& proj);

// ─── Input plumbing ──────────────────────────────────────────────────────
// (see also Imm.h::set_input)

}  // namespace psynder::ui::imm
