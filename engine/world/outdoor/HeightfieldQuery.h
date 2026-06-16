// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/HeightfieldQuery.h — deterministic CPU gameplay/physics
// queries against a heightmap (the SAME HeightmapDesc data the renderer uses).
// This is the authoritative outdoor-terrain collision the agents / match /
// physics need on a Battlefield-light map: ground height, surface normal, a
// downward/`oblique` raycast, and a ground-clamp.
//
// Determinism: the runtime queries (height / normal / raycast / clamp) are pure
// ALGEBRAIC operations (+ - * /) plus sqrt over the u16 height data — IEEE-754
// deterministic across arm64 / x86_64 / MSVC under the lane's strict-FP flags,
// so they are safe on the lockstep tick. `generate_hills` uses transcendentals
// (sin) and is therefore OFFLINE CONTENT generation only: its output (the u16
// array) is the authoritative map data shared by every client — clients must
// load that serialized data, NOT regenerate it (libm sin is not guaranteed
// identical across platforms). Metric: world X/Z/Y in metres.

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "math/Math.h"
#include "core/Types.h"

#include <vector>

namespace psynder::world::outdoor {

// Bilinear terrain height (metres) at world (wx, wz). 0 outside the map.
f32 terrain_height(const HeightmapDesc& h, f32 wx, f32 wz) noexcept;

// Smooth unit surface normal at world (wx, wz) via central differences of the
// bilinear field (+Y up on flat ground).
math::Vec3 terrain_normal(const HeightmapDesc& h, f32 wx, f32 wz) noexcept;

struct RayHit {
    bool       hit = false;
    f32        t = 0.0f;            // distance along dir to the surface (metres)
    math::Vec3 point{0.0f, 0.0f, 0.0f};
    math::Vec3 normal{0.0f, 1.0f, 0.0f};
};

// March `origin + t*dir` over t in [0, max_t] (metres) and return the first
// surface crossing (where the ray passes from above the terrain to on/below it).
// `step` is the march increment in metres; <= 0 picks a sensible default from
// the texel spacing. The crossing is bisection-refined. Deterministic.
RayHit terrain_raycast(const HeightmapDesc& h, math::Vec3 origin, math::Vec3 dir,
                       f32 max_t, f32 step = 0.0f) noexcept;

// Snap a position's Y to the terrain surface + foot_offset (e.g. a capsule's
// foot-to-origin height). Returns the clamped position.
math::Vec3 clamp_to_ground(const HeightmapDesc& h, math::Vec3 pos,
                           f32 foot_offset = 0.0f) noexcept;

// OFFLINE content helper (see header note): deterministic rolling-hills terrain
// as a sum of sines (no RNG; `seed` shifts the phase). Fills `out` with
// size_x*size_z u16 samples in [0, amplitude_units]. Same platform + args =>
// identical output; cross-platform parity is NOT guaranteed (transcendentals),
// so serialize the result rather than regenerating it per client.
void generate_hills(std::vector<u16>& out, u32 size_x, u32 size_z, u32 seed,
                    f32 amplitude_units = 8000.0f) noexcept;

}  // namespace psynder::world::outdoor
