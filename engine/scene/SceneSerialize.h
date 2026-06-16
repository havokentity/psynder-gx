// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/scene/SceneSerialize.h
//
// Lane 06 — portable scene save/load: serialize a scene::World to a flat
// little-endian byte blob and load it back into a fresh World, recreating the
// entities and their components with bit-identical values. This is the
// foundation the WYSIWYG editor needs to persist authored levels.
//
// SCOPE / COMPONENT SET
// ---------------------
// We serialize every entity carrying a TransformWS (the spatial anchor every
// renderable / physical prop has) plus the other canonical scene-lane content
// components on that entity:
//
//   * TransformWS   (GxComponents.h)    — model-to-world + previous-frame matrix
//   * Collider      (SceneComponents.h) — shape kind + half-extents
//   * RenderMaterial(SceneComponents.h) — albedo / roughness / metallic / emissive
//   * DynamicBody   (SceneComponents.h) — rigid-body mass / friction / restitution
//
// These four are exactly what spawn_prop / spawn_dynamic_prop author, so a
// round-trip reproduces a designer-built scene. They are all trivially-copyable
// POD reachable from the scene lane WITHOUT a new link dependency.
//
// The gameplay POD set (Health, Armor, Team, Weapon, Score, Respawnable lives
// in engine/gameplay/GameplayComponents.h) is intentionally NOT serialized
// here: the scene lane does not link psynder_gameplay (see
// engine/scene/CMakeLists.txt — LINKS psynder_core psynder_math psynder_jobs
// psynder_asset). Adding those is a follow-up that belongs in the gameplay lane
// (or behind a new link), so this module stays inside its lane.
//
// BYTE FORMAT (all integers explicit little-endian, NO struct memcpy)
// -------------------------------------------------------------------
//   Header (12 bytes):
//     u32  magic         = 'P','S','C','N' little-endian  (kMagic)
//     u32  version       = kVersion
//     u32  entity_count  = number of entity records that follow
//   Per-entity record (variable length):
//     u32  component_mask  — bit i set => component i present (see ComponentBit)
//     payload for each present component, in ascending bit order:
//        TransformWS    : 32 × f32 (mtw[16] then prev_mtw[16])           = 128 B
//        Collider       : 1 × u32 (ShapeKind) + 3 × f32 (half_extents)    = 16 B
//        RenderMaterial : 9 × f32 (albedo3, roughness, metallic,
//                                  emissive3, emissive_intensity)         = 36 B
//        DynamicBody    : 3 × f32 (mass_kg, friction, restitution)        = 12 B
//
// Floats are written via their exact IEEE-754 bit pattern (bit_cast to u32,
// then LE bytes), so a save/load round-trip is bit-identical. Entity iteration
// is over TransformWS chunks in archetype/chunk order, which is stable for a
// given build, so serializing the same scene twice yields identical bytes.
//
// Entity ids themselves are NOT stored: deserialize calls out.create() fresh,
// so loaded ids are dense and start from the fresh World's allocator. The
// editor refers to props by scene-record order, not by raw Entity handle.

#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::scene {

class World;

// FourCC 'PSCN' in little-endian byte order: 'P'=0x50 'S'=0x53 'C'=0x43 'N'=0x4E.
inline constexpr u32 kSceneMagic   = 0x4E435350u;  // bytes: 50 53 43 4E
inline constexpr u32 kSceneVersion = 1u;

// Component presence bits in the per-entity mask. Order is the on-wire payload
// order; never renumber an existing bit (it is the wire contract).
enum ComponentBit : u32 {
    kBitTransformWS    = 1u << 0,
    kBitCollider       = 1u << 1,
    kBitRenderMaterial = 1u << 2,
    kBitDynamicBody    = 1u << 3,
};

// Serialize every entity carrying a TransformWS (plus its Collider /
// RenderMaterial / DynamicBody, when present) into `out`. `out` is cleared
// first. Deterministic: the same scene produces identical bytes.
void serialize_scene(World& w, std::vector<u8>& out);

// Recreate the serialized scene into `out` (a fresh World is expected; this
// only appends entities). Returns false on a malformed buffer: too short for
// the header, bad magic, unsupported version, an unknown mask bit, or a record
// payload that runs past the end of the buffer. On false, `out` may hold the
// entities decoded before the error was detected.
bool deserialize_scene(std::span<const u8> bytes, World& out);

}  // namespace psynder::scene
