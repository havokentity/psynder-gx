// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ui/imm/detail/GpuBatch.h
//
// Lane 21 — CPU-side draw-command accumulator for the immediate-mode
// overlay.  The public drawing helpers (filled_rect, line, circle, etc.)
// write into this structure during a frame; `flush_batch()` uploads the
// resulting vertex + index data to a HostVisible staging buffer and emits a
// single drawcall through psy::gpu::CmdBuffer.
//
// The batch owns NO GPU objects itself — it holds non-owning pointers
// supplied by the host at begin_frame time (the CmdBuffer, the UI
// GraphicsPipeline handle, and the HostVisible vertex buffer that was
// created at startup).  This keeps mid-frame allocation out of the frame
// loop (DESIGN §4.4).
//
// Wire format: each vertex is { f32 x, f32 y, u32 rgba }.  A trivial
// passthrough vertex shader transforms (x,y) from pixel coords to [-1,1]
// clip space using a push-constant (viewport_rcp: {2/w, 2/h}).  The
// fragment shader outputs the interpolated vertex colour, alpha-blended
// via the pipeline's premultiplied-alpha blend state.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <array>
#include <cstring>

namespace psynder::ui::imm::detail {

// ─── Vertex layout ───────────────────────────────────────────────────────
//
// Intentionally flat — no normal, no UV — so the overlay pipeline stays
// maximally thin.  Colour is packed RGBA8 (0xRRGGBBAA, same convention as
// Pixel.h).

struct ImmVertex {
    f32 x, y;
    f32 u, v;
    u32 rgba;    // packed 0xRRGGBBAA
};

// ─── Capacity constants ─────────────────────────────────────────────────
//
// Sized for the heaviest in-game overlay scenario: ~50 perf-graph bars +
// three gizmo rings + a handful of selection boxes + text labels.  The
// underlying HostVisible buffer must be allocated with at least this size.

inline constexpr u32 kImmMaxVertices = 65536;
inline constexpr u32 kImmMaxIndices  = 196608;  // 3× vertices (worst case tris)

// ─── GpuBatch ────────────────────────────────────────────────────────────

struct GpuBatch {
    // ── Frame-to-frame persistent references (host sets at begin_frame) ──
    psynder::gpu::CmdBuffer*     cmd        = nullptr;  // borrow; not owned
    psynder::shader::PipelineHandle pipeline{};          // copied; immutable once created
    psynder::gpu::Buffer*        vb         = nullptr;  // HostVisible vertex buffer
    psynder::gpu::Buffer*        ib         = nullptr;  // HostVisible index buffer
    psynder::gpu::Texture*       texture    = nullptr;  // borrowed atlas/white texture
    psynder::gpu::Sampler*       sampler    = nullptr;  // borrowed atlas sampler
    f32                          vp_width   = 1.0f;
    f32                          vp_height  = 1.0f;
    f32                          shader_time_seconds = 0.0f;
    f32                          shader_effect_strength = 0.0f;
    f32                          white_u = 0.5f;
    f32                          white_v = 0.5f;

    // ── Per-frame CPU scratch ─────────────────────────────────────────────
    std::array<ImmVertex, kImmMaxVertices> verts{};
    std::array<u32,       kImmMaxIndices>  indices{};
    u32 vert_count  = 0;
    u32 index_count = 0;

    // ── State ────────────────────────────────────────────────────────────
    bool frame_open = false;

    // ── Public helpers ───────────────────────────────────────────────────

    void begin(psynder::gpu::CmdBuffer* c,
               psynder::shader::PipelineHandle p,
               psynder::gpu::Buffer* vertex_buf,
               psynder::gpu::Buffer* index_buf,
               f32 width, f32 height) noexcept {
        cmd        = c;
        pipeline   = p;
        vb         = vertex_buf;
        ib         = index_buf;
        texture    = nullptr;
        sampler    = nullptr;
        vp_width   = (width  > 0.0f) ? width  : 1.0f;
        vp_height  = (height > 0.0f) ? height : 1.0f;
        shader_time_seconds = 0.0f;
        shader_effect_strength = 0.0f;
        vert_count  = 0;
        index_count = 0;
        frame_open  = true;
    }

    // Returns false and silently drops the geometry if the batch is full.
    bool reserve(u32 extra_verts, u32 extra_indices) noexcept {
        return (vert_count  + extra_verts  <= kImmMaxVertices) &&
               (index_count + extra_indices <= kImmMaxIndices);
    }

    void push_quad(f32 x0, f32 y0, f32 x1, f32 y1, u32 colour) noexcept {
        push_textured_quad(x0, y0, x1, y1, white_u, white_v, white_u, white_v, colour);
    }

    void push_textured_quad(f32 x0, f32 y0, f32 x1, f32 y1,
                            f32 u0, f32 v0, f32 u1, f32 v1,
                            u32 colour) noexcept {
        if (!reserve(4, 6)) return;
        const u32 base = vert_count;
        verts[vert_count++] = {x0, y0, u0, v0, colour};
        verts[vert_count++] = {x1, y0, u1, v0, colour};
        verts[vert_count++] = {x1, y1, u1, v1, colour};
        verts[vert_count++] = {x0, y1, u0, v1, colour};
        indices[index_count++] = base + 0;
        indices[index_count++] = base + 1;
        indices[index_count++] = base + 2;
        indices[index_count++] = base + 0;
        indices[index_count++] = base + 2;
        indices[index_count++] = base + 3;
    }

    void push_line_pixel(i32 x, i32 y, u32 colour) noexcept {
        // Each pixel is a 1×1 quad.
        push_quad(static_cast<f32>(x),
                  static_cast<f32>(y),
                  static_cast<f32>(x) + 1.0f,
                  static_cast<f32>(y) + 1.0f,
                  colour);
    }

    void flush() noexcept;
};

// The single batch instance for the calling thread (always the render
// thread).  Exposed so Draw.h / Font.h helpers can push geometry directly
// without threading a pointer through every call site.
inline GpuBatch& batch() noexcept {
    static GpuBatch b{};
    return b;
}

// ─── flush implementation ────────────────────────────────────────────────
//
// Uploads the CPU scratch to the HostVisible buffers and emits a drawcall.
// Called exactly once at end_frame() — no other code path invokes it.
//
// The implementation is deliberately kept in the header (inline) so the
// unit-test harness can include this file and call flush() without needing
// the GPU backend initialised; flush() is a no-op when cmd == nullptr.

inline void GpuBatch::flush() noexcept {
    frame_open = false;
    if (!cmd || vert_count == 0 || !vb || !ib) return;

    // Upload vertices
    {
        void* mapped = psynder::gpu::buffer_map(vb);
        if (mapped) {
            std::memcpy(mapped, verts.data(),
                        sizeof(ImmVertex) * vert_count);
            psynder::gpu::buffer_unmap(vb);
        }
    }
    // Upload indices
    {
        void* mapped = psynder::gpu::buffer_map(ib);
        if (mapped) {
            std::memcpy(mapped, indices.data(),
                        sizeof(u32) * index_count);
            psynder::gpu::buffer_unmap(ib);
        }
    }

    if (!pipeline.valid()) return;

    struct PushConstants {
        f32 viewport_rcp[2];
        f32 time_seconds;
        f32 effect_strength;
    };
    const PushConstants pc{
        {2.0f / vp_width, 2.0f / vp_height},
        shader_time_seconds,
        shader_effect_strength,
    };

    psynder::gpu::bind_pipeline(cmd, pipeline);
    if (texture && sampler) {
        psynder::gpu::bind_texture(cmd, 0, texture, sampler);
    }
    psynder::gpu::push_constants(cmd,
                                 &pc,
                                 static_cast<u32>(sizeof(pc)),
                                 psynder::gpu::ShaderStage::AllGfx);
    psynder::gpu::bind_vertex_buffer(cmd, 0, vb, 0);
    psynder::gpu::bind_index_buffer(cmd, ib, psynder::gpu::IndexType::U32, 0);
    psynder::gpu::draw_indexed(cmd, index_count, 1, 0, 0, 0);
}

}  // namespace psynder::ui::imm::detail
