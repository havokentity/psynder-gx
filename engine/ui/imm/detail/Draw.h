// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ui/imm/detail/Draw.h
//
// Lane 21 — internal drawing primitives for the immediate-mode overlay.
//
// GX change from Psynder Wave-A: primitives no longer write into a CPU
// framebuffer.  Instead every helper pushes quads into the thread-local
// GpuBatch (see GpuBatch.h).  The Bresenham / scanline algorithms are
// retained for geometry fidelity but their output is GPU triangles, not
// individual pixels.
//
// render/Framebuffer.h is NOT included here.

#pragma once

#include "GpuBatch.h"
#include "Pixel.h"

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <cmath>

namespace psynder::ui::imm::detail {

// ─── Filled axis-aligned rectangle ───────────────────────────────────────

inline void filled_rect(math::Vec2 origin, math::Vec2 size, u32 colour) noexcept {
    if (size.x <= 0.0f || size.y <= 0.0f) return;
    batch().push_quad(origin.x,
                      origin.y,
                      origin.x + size.x,
                      origin.y + size.y,
                      colour);
}

// ─── 1-pixel rectangle outline ───────────────────────────────────────────
//
// Emitted as four 1-px-thick quads so the GPU rasterizer gives the same
// result as the old pixel-write loop.

inline void rect_outline(math::Vec2 origin, math::Vec2 size, u32 colour) noexcept {
    if (size.x <= 0.0f || size.y <= 0.0f) return;
    const f32 x0 = origin.x;
    const f32 y0 = origin.y;
    const f32 x1 = origin.x + size.x;
    const f32 y1 = origin.y + size.y;
    // Top edge
    batch().push_quad(x0, y0, x1, y0 + 1.0f, colour);
    // Bottom edge
    if (size.y > 1.0f)
        batch().push_quad(x0, y1 - 1.0f, x1, y1, colour);
    // Left edge (inner rows only — top/bottom already cover corners)
    if (size.y > 2.0f)
        batch().push_quad(x0, y0 + 1.0f, x0 + 1.0f, y1 - 1.0f, colour);
    // Right edge
    if (size.x > 1.0f && size.y > 2.0f)
        batch().push_quad(x1 - 1.0f, y0 + 1.0f, x1, y1 - 1.0f, colour);
}

// ─── Bresenham line (emitted as 1×1 pixel quads) ────────────────────────

inline void line(math::Vec2 a, math::Vec2 b, u32 colour) noexcept {
    i32 x0 = static_cast<i32>(std::lround(a.x));
    i32 y0 = static_cast<i32>(std::lround(a.y));
    const i32 x1 = static_cast<i32>(std::lround(b.x));
    const i32 y1 = static_cast<i32>(std::lround(b.y));
    const i32 dx =  std::abs(x1 - x0);
    const i32 dy = -std::abs(y1 - y0);
    const i32 sx = x0 < x1 ? 1 : -1;
    const i32 sy = y0 < y1 ? 1 : -1;
    i32 err = dx + dy;
    while (true) {
        batch().push_line_pixel(x0, y0, colour);
        if (x0 == x1 && y0 == y1) break;
        const i32 e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ─── Circle (outline or solid disk) ─────────────────────────────────────
//
// For an outline circle each octant point becomes a 1×1 pixel quad.
// For a filled disk we scanline-fill using horizontal quads per scan line.

inline void circle(math::Vec2 centre, f32 radius,
                   u32 colour, bool filled = false) noexcept {
    if (radius <= 0.0f) return;
    const i32 cx = static_cast<i32>(std::lround(centre.x));
    const i32 cy = static_cast<i32>(std::lround(centre.y));
    i32 x = static_cast<i32>(std::lround(radius));
    i32 y = 0;
    i32 err = 1 - x;

    auto hspan = [&](i32 xa, i32 xb, i32 yy) {
        if (xb < xa) std::swap(xa, xb);
        batch().push_quad(static_cast<f32>(xa),
                          static_cast<f32>(yy),
                          static_cast<f32>(xb + 1),
                          static_cast<f32>(yy + 1),
                          colour);
    };

    while (x >= y) {
        if (filled) {
            hspan(cx - x, cx + x, cy + y);
            if (y != 0) hspan(cx - x, cx + x, cy - y);
            hspan(cx - y, cx + y, cy + x);
            if (x != 0) hspan(cx - y, cx + y, cy - x);
        } else {
            batch().push_line_pixel(cx + x, cy + y, colour);
            batch().push_line_pixel(cx - x, cy + y, colour);
            batch().push_line_pixel(cx + x, cy - y, colour);
            batch().push_line_pixel(cx - x, cy - y, colour);
            batch().push_line_pixel(cx + y, cy + x, colour);
            batch().push_line_pixel(cx - y, cy + x, colour);
            batch().push_line_pixel(cx + y, cy - x, colour);
            batch().push_line_pixel(cx - y, cy - x, colour);
        }
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

// Translucent disk — uses blend_over to pre-composite the alpha so the
// GPU pipeline sees a correct premultiplied RGBA value.
inline void blended_disk(math::Vec2 centre, f32 radius, u32 colour) noexcept {
    // Pre-multiply alpha: scale RGB channels by A/255.
    const u32 src_a = colour & 0xFFu;
    if (src_a == 0u) return;
    const u32 r = ((colour >> 24) & 0xFFu) * src_a / 255u;
    const u32 g = ((colour >> 16) & 0xFFu) * src_a / 255u;
    const u32 b = ((colour >> 8 ) & 0xFFu) * src_a / 255u;
    const u32 premul = (r << 24) | (g << 16) | (b << 8) | src_a;
    // Filled disk with the premultiplied colour; the GPU blend mode
    // (premul-src, one-minus-src-alpha) will composite it correctly.
    circle(centre, radius, premul, /*filled=*/true);
}

}  // namespace psynder::ui::imm::detail
