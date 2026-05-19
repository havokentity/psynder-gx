// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/asset/TextureCookerGx.h
//
// Wave A FROZEN public-header CONTRACT (lane 05-asset).
//
// GPU texture cooker stubs for Psynder-GX. Implementations are filled in
// by Wave B work (or by the `lm_cook_gx` offline tool when it lands at
// M2). These declarations are what other lanes call to convert raw image
// bytes into GPU-ready block-compressed mip chains.
//
// Block-compression formats supported (per DESIGN-PSYNDER-GX.md §10.7):
//   BC1 — 4 bpp, RGB, opaque. Albedo / Roughness packed.
//   BC3 — 8 bpp, RGBA with alpha. Normal map alternative (when channels are tight).
//   BC7 — 8 bpp, high-quality RGBA. Default for hero textures + normal maps.
//   ASTC — variable bpp, used on Apple Silicon (Metal) where supported.
//
// Compression libraries (vendored): `bc7enc`, `ispc_texcomp`, or similar.
// Cook quality is tunable; defaults aim at "ship build" quality (slower
// than dev, higher fidelity).
//
// All cookers are pure functions on raw bytes — they do NOT touch the GPU
// device. The output is a memory blob ready to be written to a .lmpak
// archive or uploaded via psy::gpu::create_texture(...).

#pragma once

#include "core/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace psynder::asset {

// ─── Format selectors ────────────────────────────────────────────────────
enum class GxBlockFormat : std::uint8_t {
    Bc1,            // RGB (DXT1)
    Bc3,            // RGBA (DXT5)
    Bc7,            // High-quality RGBA — default
    Astc4x4,        // Apple Silicon preferred
    Astc8x8,        // Apple Silicon lower-quality
};

enum class GxCookQuality : std::uint8_t {
    Fast,           // dev iteration
    Balanced,       // default ship
    Slow,           // ship hero textures
};

// ─── Input descriptor ────────────────────────────────────────────────────
struct GxRgba8Image {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    // Tightly packed RGBA8 (no row padding). Size = width * height * 4.
    std::span<const std::uint8_t> rgba8_pixels;
    bool is_srgb = true; // typical for albedo; false for normal/roughness
};

// ─── Output: a sequence of compressed mip levels ─────────────────────────
struct GxCookedMip {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bytes; // block-compressed payload
};

struct GxCookedTexture {
    GxBlockFormat format = GxBlockFormat::Bc7;
    bool is_srgb = true;
    std::vector<GxCookedMip> mips; // mips[0] = base, descending
};

// ─── Cooker entry points (STUBS — Wave B implementation) ─────────────────
// Each cooker generates a full mip pyramid down to 1×1 unless
// `max_mip_count` is set. Stub bodies return an empty texture; Wave B
// fills in the real compressors.

GxCookedTexture cook_bc1 (const GxRgba8Image&, GxCookQuality = GxCookQuality::Balanced,
                          std::uint32_t max_mip_count = 0);
GxCookedTexture cook_bc3 (const GxRgba8Image&, GxCookQuality = GxCookQuality::Balanced,
                          std::uint32_t max_mip_count = 0);
GxCookedTexture cook_bc7 (const GxRgba8Image&, GxCookQuality = GxCookQuality::Balanced,
                          std::uint32_t max_mip_count = 0);
GxCookedTexture cook_astc(const GxRgba8Image&, GxBlockFormat astc_variant = GxBlockFormat::Astc4x4,
                          GxCookQuality = GxCookQuality::Balanced,
                          std::uint32_t max_mip_count = 0);

// ─── Helpers ─────────────────────────────────────────────────────────────
constexpr bool gx_block_format_is_srgb_capable(GxBlockFormat f) noexcept {
    return f == GxBlockFormat::Bc1 || f == GxBlockFormat::Bc3 ||
           f == GxBlockFormat::Bc7 || f == GxBlockFormat::Astc4x4 ||
           f == GxBlockFormat::Astc8x8;
}

// Decompose a block-compressed mip pyramid byte size (sum of all levels).
std::size_t gx_cooked_texture_total_bytes(const GxCookedTexture&) noexcept;

} // namespace psynder::asset
