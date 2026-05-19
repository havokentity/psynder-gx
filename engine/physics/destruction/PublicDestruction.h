// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/destruction/PublicDestruction.h
//
// Lane 17 — Destruction module public-header CONTRACT (Wave 0).
//
// Chunk-based pre-fractured destruction with structural-integrity graph.
// Authoring is offline (Houdini / Blender / dedicated tools); the engine
// consumes pre-fractured asset bundles + a connectivity graph with
// per-joint strength thresholds. Real units: forces in N, joint strengths
// in N (or N·m for torque), chunk masses in kg from authored density.
//
// See DESIGN-PSYNDER-GX.md §10.1 (custom destruction).

#pragma once

#include <cstdint>

namespace psynder::physics::destruction {

struct Asset;       // baked .pdfa (Psynder destruction asset) file
struct Instance;    // a live instance in the world

struct AssetDesc {
    const char* asset_path;     // VFS path to .pdfa
};

Asset* load_asset(const AssetDesc&);
void   free_asset(Asset*);

struct InstanceDesc {
    Asset* asset;
    float  world_pos[3];
    float  world_quat[4];
};

Instance* spawn(struct ::psynder::physics::World*, const InstanceDesc&);
void      despawn(Instance*);

// Apply an impulse to a specific chunk (e.g. bullet hit). Cascade is
// resolved internally; structural-integrity propagation follows the
// authored joint-strength graph. Returns the number of chunks that broke
// free as a result of this impulse.
struct Impulse {
    std::uint32_t chunk_id;       // chunk index in asset
    float         direction[3];   // unit vector
    float         magnitude_n;    // Newtons
    float         contact_point[3];
};
std::uint32_t apply_impulse(Instance*, const Impulse&);

// Per-frame tick: progress the cascade simulation. O(chunks-affected).
void tick(Instance*, float dt);

} // namespace psynder::physics::destruction
