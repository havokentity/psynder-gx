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
#include <type_traits>

#include "core/Types.h"

namespace psynder::physics {
struct World;
}

namespace psynder::physics::destruction {

struct Asset;     // baked .pdfa (Psynder destruction asset) file
struct Instance;  // a live instance in the world

// ─── Lag-comp / demo world-state contract ───────────────────────────────
//
// Flat POD wire layout for deterministic destruction state. Lane 18 stores
// this as bytes in its rewind ring, but the type now lives in lane 17 so the
// payload is no longer "opaque" by contract. Variable-length sections follow
// WorldStateHeader in this order:
//
//   InstanceStateRecord[instance_count]
//   u64 chunk_broken_words[chunk_word_count]
//   u64 live_joint_words[joint_word_count]
//   DebrisStateRecord[debris_count]
//
// Bit word order is little-endian by chunk/joint index: bit (i & 63) in word
// (i >> 6). Per AGENTS.md, public-header changes require orchestrator
// mediation; GitHub issue #1 is the coordinating issue for this contract.
inline constexpr u32 kWorldStateMagic = (static_cast<u32>('D') << 24) | (static_cast<u32>('S') << 16) |
                                        (static_cast<u32>('0') << 8) | (static_cast<u32>('1') << 0);
inline constexpr u16 kWorldStateVersion = 1;
inline constexpr u32 kMaxChunksPerInstance = 512;

struct WorldStateHeader {
    u32 magic;             // == kWorldStateMagic
    u16 version;           // == kWorldStateVersion
    u16 instance_count;    // number of InstanceStateRecord entries
    u32 chunk_word_count;  // total u64 broken-chunk words across instances
    u32 joint_word_count;  // total u64 live-joint words across instances
    u32 debris_count;      // number of DebrisStateRecord entries
    u32 reserved0;         // future flags; must be 0 for version 1
};

struct InstanceStateRecord {
    u32 instance_id;       // stable destruction instance id
    u32 asset_id;          // stable cooked-asset id/hash
    u32 first_chunk_word;  // offset into chunk_broken_words[]
    u32 chunk_word_count;  // enough words for authored chunk_count
    u32 first_joint_word;  // offset into live_joint_words[]
    u32 joint_word_count;  // enough words for authored joint_count
    u32 first_debris;      // offset into DebrisStateRecord[]
    u32 debris_count;      // debris records owned by this instance
};

struct DebrisStateRecord {
    u32 instance_id;
    u32 chunk_id;
    f32 pos_m[3];
    f32 vel_m_s[3];
    f32 quat_xyzw[4];
    u32 flags;
};

static_assert(std::is_trivially_copyable_v<WorldStateHeader>);
static_assert(std::is_trivially_copyable_v<InstanceStateRecord>);
static_assert(std::is_trivially_copyable_v<DebrisStateRecord>);
static_assert(std::is_standard_layout_v<WorldStateHeader>);
static_assert(std::is_standard_layout_v<InstanceStateRecord>);
static_assert(std::is_standard_layout_v<DebrisStateRecord>);
static_assert(sizeof(WorldStateHeader) == 24);
static_assert(sizeof(InstanceStateRecord) == 32);
static_assert(sizeof(DebrisStateRecord) == 52);

constexpr usize world_state_bytes(u32 instance_count,
                                  u32 chunk_word_count,
                                  u32 joint_word_count,
                                  u32 debris_count) noexcept {
    return sizeof(WorldStateHeader) +
           static_cast<usize>(instance_count) * sizeof(InstanceStateRecord) +
           static_cast<usize>(chunk_word_count) * sizeof(u64) +
           static_cast<usize>(joint_word_count) * sizeof(u64) +
           static_cast<usize>(debris_count) * sizeof(DebrisStateRecord);
}

// Non-owning byte view over a buffer laid out as described above. Writers in
// lane 17 own allocation/serialization; lane 18 deep-copies these bytes into
// its rewind ring and fills caller-owned output buffers on rewind.
struct WorldState {
    void* data = nullptr;
    usize size = 0;
};

struct AssetDesc {
    const char* asset_path;  // VFS path to .pdfa
};

Asset* load_asset(const AssetDesc&);
void free_asset(Asset*);

struct InstanceDesc {
    Asset* asset;
    float world_pos[3];
    float world_quat[4];
};

Instance* spawn(::psynder::physics::World*, const InstanceDesc&);
void despawn(Instance*);

// Apply an impulse to a specific chunk (e.g. bullet hit). Cascade is
// resolved internally; structural-integrity propagation follows the
// authored joint-strength graph. Returns the number of chunks that broke
// free as a result of this impulse.
struct Impulse {
    std::uint32_t chunk_id;  // chunk index in asset
    float direction[3];      // unit vector
    float magnitude_n;       // Newtons
    float contact_point[3];
};
std::uint32_t apply_impulse(Instance*, const Impulse&);

// Per-frame tick: progress the cascade simulation. O(chunks-affected).
void tick(Instance*, float dt);

}  // namespace psynder::physics::destruction
