// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/raster/Raster.h — Wave 0 COMPATIBILITY SHIM.
//
// Psynder-GX's renderer is GPU-driven; there is no CPU rasterizer hot
// path. This header exists ONLY so vendored Psynder code (ui/rml,
// world/bsp, world/outdoor) that historically depended on the CPU
// raster's data types keeps compiling during Wave 0.
//
// Lane agents (12 world-bsp, 13 world-outdoor, 21 ui) should refactor
// their references onto psy::gpu / psy::render::pipeline. This file may
// be deleted once those refactors land.
//
// The full original CPU Rasterizer source lives under
// third_party/psynder-refs/render-raster-cpu/ for study.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

namespace psynder::render::raster {

struct MaterialTag {};
using MaterialId = Handle<MaterialTag>;

struct TextureTag {};
using TextureId = Handle<TextureTag>;

struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec2 lightmap_uv;
    u32        color = 0xFFFFFFFFu;
};

struct DrawItem {
    const Vertex* vertices     = nullptr;
    u32           vertex_count = 0;
    const u32*    indices      = nullptr;
    u32           index_count  = 0;
    math::Mat4    model;
    MaterialId    material;
    u8            flags        = 0;
};

struct ViewState {
    math::Mat4   view;
    math::Mat4   projection;
    Framebuffer  target;
    u32          tile_w = 64;
    u32          tile_h = 64;
};

// No-op rasterizer shim. Calls are accepted but produce no output.
// Lane 09 (render-pipeline) replaces all callers with GPU draws.
class Rasterizer {
public:
    static Rasterizer& Get();
    void begin_frame(const ViewState&) {}
    void submit(const DrawItem&) {}
    void end_frame() {}
};

inline void clear_framebuffer(Framebuffer&, u32) noexcept {}

} // namespace psynder::render::raster
