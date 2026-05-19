// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/Framebuffer.h — Wave 0 COMPATIBILITY SHIM.
//
// Psynder-GX's renderer is GPU-driven and does not use a CPU framebuffer
// for its main path. This header exists ONLY so vendored Psynder UI and
// editor-overlay code (which legitimately need a CPU bitmap target for
// in-viewport overlay rendering) keeps compiling during Wave 0.
//
// Lane agents (12 world-bsp, 13 world-outdoor, 21 ui, 23/24/25 platform)
// should refactor their references to use psy::gpu::Texture from
// engine/gpu/PublicGpu.h. This file may be deleted once those refactors
// land.

#pragma once

#include "core/Types.h"

namespace psynder::render {

enum class PixelFormat : u32 {
    RGBA8,
    BGRA8,
    RGB565,
    Paletted8,
};

// CPU-side bitmap descriptor — used by the in-viewport overlay (perf
// graphs, hitbox viz, manipulator gizmos) where rendering happens on the
// CPU and the result is uploaded to a GPU texture once per frame.
struct Framebuffer {
    u32         width  = 0;
    u32         height = 0;
    PixelFormat format = PixelFormat::RGBA8;
    u32         pitch  = 0;       // bytes per row
    u8*         pixels = nullptr;
    u32*        depth  = nullptr; // optional Z buffer (compat with vendored Psynder UI)

    constexpr usize byte_size() const noexcept {
        return static_cast<usize>(pitch) * height;
    }
};

// Compat type used by vendored Psynder platform shim. In GX this is just
// a marker — actual present runs through psy::gpu::end_frame().
struct PresentRequest {
    const Framebuffer* fb       = nullptr;
    u32                window_w = 0;
    u32                window_h = 0;
    bool               vsync    = true;
};

} // namespace psynder::render
