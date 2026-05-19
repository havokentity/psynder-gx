// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — outdoor heightmap world.
//
// **Psynder-GX deviation (ADR-GX-008):** the `HeightmapRaymarch` backend
// below is DEPRECATED for Psynder-GX. GX is GPU-driven; CDLOD-mesh
// rasterized through psy::render::pipeline is the only outdoor path.
// The raymarcher code is retained in this lane for Wave-0 compat only,
// to keep the vendored Psynder layer linkable. Lane 13 in a follow-up
// pass removes the raymarcher; lane 09 (render-pipeline) never wires
// `HeightmapRaymarch` into the GPU draw stream.
//
// Notes carried from sister Psynder: two backends (CDLOD mesh + heightmap
// raymarch) per ADR-008 in the Psynder design. Lane 13 owns this dir in
// Psynder-GX (lane 11 in Psynder).

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <string_view>
#include <vector>

namespace psynder::world::outdoor {

enum class TerrainBackend : u8 {
    PolygonCDLOD,    // default for NFS-style racing tracks
    HeightmapRaymarch,  // default for IGI / Delta Force-style tactical FPS
};

struct HeightmapDesc {
    u32        size_x      = 0;        // texels along world X
    u32        size_z      = 0;        // texels along world Z
    f32        spacing     = 1.0f;     // metres per texel
    f32        height_scale= 1.0f;     // metres per heightmap unit
    const u16* heights     = nullptr;  // 16-bit height samples
};

struct SplineRoadSegment {
    math::Vec3 p0, p1, p2, p3;         // cubic Bezier control points
    f32        half_width = 4.0f;
    f32        banking_rad = 0.0f;
};

class TerrainMesh {
public:
    void build(const HeightmapDesc& desc);
    void render_cdlod(const math::Mat4& view, const math::Mat4& proj) const;
};

class TerrainRaymarch {
public:
    void set_heightmap(const HeightmapDesc& desc);
    // Lane 11 implements per-column ray march into the framebuffer + Z.
    void render(const math::Mat4& view, const math::Mat4& proj) const;
};

void load_spline_track(std::string_view virtual_path,
                       std::vector<SplineRoadSegment>& segments_out);

}  // namespace psynder::world::outdoor
