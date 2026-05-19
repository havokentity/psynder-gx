// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ui_imm_rect.cpp
//
// Unit tests for the immediate-mode UI drawing helpers (GX edition).
//
// GX change from Psynder Wave-A: the IMM drawing primitives push quads into
// a GpuBatch rather than writing pixels to a CPU Framebuffer.  The old test
// drove framebuffer-write pixel counts; this version validates the
// quad-geometry counts emitted into the batch, which is the GX API surface.
//
// The GpuBatch's flush() is a no-op when cmd == nullptr (see GpuBatch.h),
// so the batch accumulates quads in the CPU scratch arrays and we can
// inspect vert_count / index_count directly without a live GPU device.

#include "ui/imm/detail/Draw.h"
#include "ui/imm/detail/GpuBatch.h"
#include "ui/imm/detail/Pixel.h"

#include "core/Types.h"
#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>

using psynder::u32;
using psynder::math::Vec2;
namespace imm = psynder::ui::imm::detail;

namespace {

// Reset the thread-local batch before each test so cases are independent.
void reset_batch() {
    auto& b = imm::batch();
    b.vert_count  = 0;
    b.index_count = 0;
    b.frame_open  = false;
    // cmd stays nullptr — flush() becomes a no-op, geometry stays in scratch.
}

} // namespace

TEST_CASE("ui_imm: filled_rect pushes one quad (4 verts, 6 indices)",
          "[ui_imm][filled_rect]") {
    reset_batch();
    imm::filled_rect(Vec2{4.0f, 2.0f}, Vec2{3.0f, 2.0f},
                     imm::rgba(0xAA, 0xBB, 0xCC, 0xDD));

    REQUIRE(imm::batch().vert_count  == 4u);
    REQUIRE(imm::batch().index_count == 6u);

    // Colour propagates into all four vertices.
    const u32 expected = imm::rgba(0xAA, 0xBB, 0xCC, 0xDD);
    for (u32 i = 0; i < 4u; ++i) {
        REQUIRE(imm::batch().verts[i].rgba == expected);
    }
}

TEST_CASE("ui_imm: filled_rect with zero or negative size emits nothing",
          "[ui_imm][filled_rect]") {
    reset_batch();
    imm::filled_rect(Vec2{0.0f, 0.0f}, Vec2{0.0f, 4.0f}, 0xFFFFFFFFu);
    REQUIRE(imm::batch().vert_count == 0u);

    reset_batch();
    imm::filled_rect(Vec2{0.0f, 0.0f}, Vec2{4.0f, -1.0f}, 0xFFFFFFFFu);
    REQUIRE(imm::batch().vert_count == 0u);
}

TEST_CASE("ui_imm: rect_outline with size >= 3 pushes 4 edge quads",
          "[ui_imm][rect_outline]") {
    // A 6x4 rect generates: top + bottom + left + right = 4 quads.
    // Each quad = 4 verts + 6 indices → 16 verts, 24 indices.
    reset_batch();
    imm::rect_outline(Vec2{2.0f, 3.0f}, Vec2{6.0f, 4.0f}, 0xFFFFFFFFu);

    REQUIRE(imm::batch().vert_count  == 16u);
    REQUIRE(imm::batch().index_count == 24u);
}

TEST_CASE("ui_imm: rect_outline 1x1 pushes only the top edge quad",
          "[ui_imm][rect_outline]") {
    // size.y == 1 → no bottom edge (guarded by size.y > 1), no side edges.
    // Only the top quad = 4 verts, 6 indices.
    reset_batch();
    imm::rect_outline(Vec2{5.0f, 5.0f}, Vec2{1.0f, 1.0f}, 0xFFFFFFFFu);

    REQUIRE(imm::batch().vert_count  == 4u);
    REQUIRE(imm::batch().index_count == 6u);
}

TEST_CASE("ui_imm: rect_outline 2x2 pushes top + bottom (no side strips)",
          "[ui_imm][rect_outline]") {
    // size.y == 2 → bottom edge present (size.y > 1), but NO side strips
    // (guarded by size.y > 2).
    // 2 quads = 8 verts, 12 indices.
    reset_batch();
    imm::rect_outline(Vec2{3.0f, 7.0f}, Vec2{4.0f, 2.0f}, 0xFFFFFFFFu);

    REQUIRE(imm::batch().vert_count  == 8u);
    REQUIRE(imm::batch().index_count == 12u);
}

TEST_CASE("ui_imm: line between two non-equal points emits at least one pixel quad",
          "[ui_imm][line]") {
    reset_batch();
    imm::line(Vec2{1.0f, 1.0f}, Vec2{6.0f, 4.0f}, 0xFFFFFFFFu);

    // A diagonal line must emit at least 4 pixel quads.
    REQUIRE(imm::batch().vert_count  >= 16u);
    REQUIRE(imm::batch().index_count >= 24u);
    // Vert count must be a multiple of 4 (each pixel is one quad).
    REQUIRE((imm::batch().vert_count % 4u) == 0u);
}

TEST_CASE("ui_imm: line between identical points emits exactly one pixel quad",
          "[ui_imm][line]") {
    reset_batch();
    imm::line(Vec2{5.0f, 5.0f}, Vec2{5.0f, 5.0f}, 0xFFFFFFFFu);

    REQUIRE(imm::batch().vert_count  == 4u);
    REQUIRE(imm::batch().index_count == 6u);
}

TEST_CASE("ui_imm: rgba packing round-trips R/G/B/A correctly",
          "[ui_imm][pixel]") {
    const u32 v = imm::rgba(0xFF, 0x80, 0x40, 0x20);
    REQUIRE(((v >> 24) & 0xFFu) == 0xFFu);  // R
    REQUIRE(((v >> 16) & 0xFFu) == 0x80u);  // G
    REQUIRE(((v >>  8) & 0xFFu) == 0x40u);  // B
    REQUIRE(( v        & 0xFFu) == 0x20u);  // A
}

TEST_CASE("ui_imm: blend_over with opaque source returns source unchanged",
          "[ui_imm][blend]") {
    const u32 src = imm::rgba(0xFF, 0x00, 0x00, 0xFF);  // opaque red
    const u32 dst = imm::rgba(0x00, 0x00, 0xFF, 0xFF);  // opaque blue
    REQUIRE(imm::blend_over(src, dst) == src);
}

TEST_CASE("ui_imm: blend_over with transparent source returns dest unchanged",
          "[ui_imm][blend]") {
    const u32 src = imm::rgba(0xFF, 0x00, 0x00, 0x00);  // fully transparent
    const u32 dst = imm::rgba(0x00, 0x00, 0xFF, 0xFF);
    REQUIRE(imm::blend_over(src, dst) == dst);
}

TEST_CASE("ui_imm: blend_over 50% red over blue blends both channels",
          "[ui_imm][blend]") {
    const u32 src = imm::rgba(0xFF, 0x00, 0x00, 0x80);  // red 50%
    const u32 dst = imm::rgba(0x00, 0x00, 0xFF, 0xFF);  // opaque blue
    const u32 out = imm::blend_over(src, dst);
    const u32 out_r = (out >> 24) & 0xFFu;
    const u32 out_b = (out >>  8) & 0xFFu;
    REQUIRE(out_r > 120u);
    REQUIRE(out_r < 136u);
    REQUIRE(out_b > 120u);
    REQUIRE(out_b < 136u);
}
