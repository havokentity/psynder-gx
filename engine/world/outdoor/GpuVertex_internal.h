// SPDX-License-Identifier: MIT
// Psynder-GX — GX-native vertex layout for outdoor world geometry.
//
// Replaces the Wave-0 compat shim render::raster::Vertex used by the CDLOD
// mesh builder and spline extruder.  The layout is intentionally identical
// to the retired shim vertex so that lane 09 (render-pipeline) can consume
// the same CdlodChunk / ExtrudedStrip data without a format conversion.
//
// Once lane 09 is live the per-field names here become the load-bearing
// contract.  Do NOT reorder or resize fields without filing an Issue against
// lane 09.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

namespace psynder::world::outdoor::detail {

// Packed per-vertex data for CDLOD terrain chunks and spline road strips.
// Consumed by lane 09 (render-pipeline) via CdlodChunk::vertices and
// ExtrudedStrip::vertices.
//
// Layout (all f32 unless noted):
//   position     — world-space XYZ (1 unit = 1 metre, DESIGN §9.2)
//   normal       — unit-length world-space normal
//   uv           — primary texture / colormap UV
//   lightmap_uv  — lightmap atlas UV (same parameterisation as uv in Wave A)
//   color        — RGBA8 packed u32; terrain uses splat weights (grass/rock/
//                  sand/snow in RGBA channels); road strips use 0xFFFFFFFF
struct OutdoorVertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec2 lightmap_uv;
    u32        color = 0xFFFFFFFFu;
};

} // namespace psynder::world::outdoor::detail
