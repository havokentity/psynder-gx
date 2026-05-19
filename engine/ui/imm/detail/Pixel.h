// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ui/imm/detail/Pixel.h
//
// Lane 21 — colour packing / blending helpers for the immediate-mode UI.
//
// GX change from Psynder Wave-A: the pixel-write and framebuffer-address
// helpers are gone — overlays no longer write into a CPU framebuffer.
// What remains is:
//   - `rgba()` packing (0xRRGGBBAA literal → u32)
//   - `blend_over()` for source-over compositing of two packed RGBA values
//     (used internally when the GPU pipeline's alpha blend is not yet wired;
//     the GPU alpha blend mode supersedes this at draw time)
//   - `iclamp()` integer clamp (used by rect and circle geometry emitters)
//
// GPU alpha blend supersedes CPU blend at draw time.

#pragma once

#include "core/Types.h"

namespace psynder::ui::imm::detail {

// Pack an RGBA8 quadruple.  Channel order: 0xRRGGBBAA in source-literal
// form — R in the high byte, A in the low byte.  This matches the vertex
// colour encoding in GpuBatch::ImmVertex and the shader's unpack order.
inline constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 0xFFu) noexcept {
    return (static_cast<u32>(r) << 24) |
           (static_cast<u32>(g) << 16) |
           (static_cast<u32>(b) << 8)  |
           static_cast<u32>(a);
}

// Source-over alpha blend of two packed RGBA8 values.  Used by the
// selection-highlight and brush-preview helpers when they need to produce
// a pre-blended colour before writing to the batch (the GPU pipeline will
// use its own blend equation, but callers that want a specific look can
// pre-blend on the CPU).
inline u32 blend_over(u32 src, u32 dst) noexcept {
    const u32 src_a = src & 0xFFu;
    if (src_a == 0xFFu) return src;
    if (src_a == 0u)    return dst;
    const u32 inv   = 255U - src_a;
    const u32 src_r = (src >> 24) & 0xFFu;
    const u32 src_g = (src >> 16) & 0xFFu;
    const u32 src_b = (src >> 8 ) & 0xFFu;
    const u32 dst_r = (dst >> 24) & 0xFFu;
    const u32 dst_g = (dst >> 16) & 0xFFu;
    const u32 dst_b = (dst >> 8 ) & 0xFFu;
    const u32 dst_a =  dst        & 0xFFu;
    const u32 out_r = (src_r * src_a + dst_r * inv) / 255U;
    const u32 out_g = (src_g * src_a + dst_g * inv) / 255U;
    const u32 out_b = (src_b * src_a + dst_b * inv) / 255U;
    const u32 out_a = src_a + (dst_a * inv) / 255U;
    return (out_r << 24) | (out_g << 16) | (out_b << 8) | out_a;
}

// Integer clamp — consolidated here so the geometry helpers don't
// accumulate sign-conversion warnings.
inline i32 iclamp(i32 v, i32 lo, i32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace psynder::ui::imm::detail
