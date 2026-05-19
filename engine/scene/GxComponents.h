// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/scene/GxComponents.h
//
// Lane 06 — PUBLIC component schema for the GX renderer family.
//
// This header is the canonical contract every render lane (09 render-pipeline,
// 10 render-rt, 11 render-post) queries the ECS against.  Freezing it here
// lets lane 09's Extract.cpp graduate from the M1 hardcoded-triangle fallback
// to real ECS-driven draw-list extraction (M2+ target, Issue lane09-003).
//
// Coding rules (DESIGN §3 + AGENTS.md):
//   - All types are trivially-copyable POD — the PSYNDER_COMPONENT macro
//     asserts this at compile time. No constructors, no vtable, no shared_ptr.
//   - Real metric units throughout: positions in metres, angles in radians,
//     luminous intensity in lumens (point) or lux (directional/ambient),
//     radiometric energy in watts where not photometric.
//   - 1 world unit == 1 metre (DESIGN §14).
//   - Column-major Mat4 (right-handed, per engine/math/Math.h convention).
//
// Lifecycle column in each component block indicates the owning system and
// the lane that queries the component in a typical frame.

#pragma once

#include "scene/World.h"   // PSYNDER_COMPONENT, psynder::scene namespace
#include "math/Math.h"     // Mat4, Vec3, Quat — psynder::math
#include "core/Types.h"    // u32, f32, bool, ...

#include <cstdint>

// ─── Asset-ID forward declarations ───────────────────────────────────────────
// Lane 05 (asset) does not yet publish typed MeshId / MaterialId handles in a
// Public*.h header.  Until lane 05 ships that contract, we define lightweight
// strongly-typed u32 wrappers here so callers can work type-safely without a
// magic-number cast.  When lane 05 finalises its public surface (see DESIGN
// §5.2), these aliases should be replaced with a single #include.
namespace psynder::asset {

struct MeshIdTag {};
struct MaterialIdTag {};

/// Opaque handle to a cooked mesh resource held by the asset VFS.
/// Backed by psynder::Handle<MeshIdTag> — a non-zero raw value is valid.
using MeshId     = ::psynder::Handle<MeshIdTag>;

/// Opaque handle to a cooked / GPU-resident material resource.
using MaterialId = ::psynder::Handle<MaterialIdTag>;

}  // namespace psynder::asset

// ─── GX render components ────────────────────────────────────────────────────
namespace psynder::scene {

// ─── TransformWS ─────────────────────────────────────────────────────────────
//
// World-space transform baked to a 4×4 column-major float matrix.
//
// Lifecycle:
//   Written by: the transform-propagation system (lane 06) each frame when the
//     scene graph dirty-flag is set.  Also written directly by physics on
//     dynamic actors (lane 15) and by the script layer (lane 20).
//   Read by:    lane 09 Extract.cpp for every renderable entity; lane 10 TLAS
//     builder for RT instances; lane 11 velocity-buffer pass (prev_mtw).
//
// Units: metres (all translation components are in metres, scale is
//   dimensionless, rotation is encoded in the upper-left 3×3 sub-matrix).
//
// Alignment: 16-byte so SIMD loads are natural.  The ECS chunk allocator
//   respects alignof(TransformWS) automatically.

PSYNDER_COMPONENT(TransformWS) {
    alignas(16) psynder::math::Mat4 mtw;   ///< model-to-world matrix (metres)
    alignas(16) psynder::math::Mat4 prev_mtw; ///< previous-frame mtw (motion vectors)
};
static_assert(sizeof(TransformWS) == 128, "TransformWS layout frozen at 128 bytes");

// ─── MeshRef ─────────────────────────────────────────────────────────────────
//
// Reference to a GPU-resident mesh resource (vertex + index buffers).
//
// Lifecycle:
//   Written by: the spawner / loader system when the entity is created.
//     Treated as immutable during gameplay; reassignment triggers a full
//     archetype migration (structural change).
//   Read by:    lane 09 Extract.cpp to feed the indirect-draw instance table.
//     Lane 09 never dereferences the handle directly; it passes it to the
//     resource cache (lane 07 gpu) for the actual buffer bind.
//
// A MeshId of raw==0 is invalid — the entity will be skipped by the extract
// pass.  Use World::remove<MeshRef>(e) to make an entity non-renderable.

PSYNDER_COMPONENT(MeshRef) {
    psynder::asset::MeshId  mesh;     ///< cooked mesh handle
    u32                     lod_bias; ///< optional LOD offset (+1 = one step coarser)
    u32                     _pad0;
};
static_assert(sizeof(MeshRef) == 12, "MeshRef layout frozen at 12 bytes");

// ─── MaterialRef ─────────────────────────────────────────────────────────────
//
// Reference to a GPU-resident PBR material (Slang parameter block + textures).
//
// Lifecycle:
//   Written by: spawner / loader on entity creation; mutable at runtime (e.g.
//     decal splatter, damage states).
//   Read by:    lane 09 OpaquePass for the per-draw material descriptor-set
//     binding.  Lane 10 RT uses it for any-hit shader (alpha testing).

PSYNDER_COMPONENT(MaterialRef) {
    psynder::asset::MaterialId material; ///< cooked material handle
    u32                        _pad0;
};
static_assert(sizeof(MaterialRef) == 8, "MaterialRef layout frozen at 8 bytes");

// ─── LightPoint ──────────────────────────────────────────────────────────────
//
// Isotropic point light.  Used by lane 09's light-cluster build pass to assign
// lights to screen-space froxels (3-D tile grid, PipelineDesc specifies the
// cluster resolution).
//
// Lifecycle:
//   Written by: gameplay / script; safe to update every frame.
//   Read by:    lane 09 LightCluster.cpp during the per-frame cluster-build
//     compute dispatch.  Lane 10 ray-generation for shadow rays (M3+).
//
// Units:
//   position   — metres (world space)
//   radius     — metres (influence sphere; attenuation reaches zero at radius)
//   color      — linear sRGB [0, 1]  (NOT gamma-encoded)
//   intensity  — lumens [lm]  (photometric)

PSYNDER_COMPONENT(LightPoint) {
    psynder::math::Vec3 position;   ///< world-space position, metres
    f32                 radius;     ///< influence sphere radius, metres (> 0)
    psynder::math::Vec3 color;      ///< linear sRGB, [0, 1] per channel
    f32                 intensity;  ///< luminous flux, lumens [lm]
};
static_assert(sizeof(LightPoint) == 32, "LightPoint layout frozen at 32 bytes");

// ─── LightDirectional ────────────────────────────────────────────────────────
//
// Infinitely-distant directional light (sun / moon).  Typically one per scene
// but the ECS supports multiple (e.g. day-night blend).
//
// Lifecycle:
//   Written by: time-of-day system or designer.  Expected to be stable across
//     many frames; set once per level-tick or on TOD change only.
//   Read by:    lane 09 forward+ light loop (sky ambient injection), lane 09
//     CSM shadow-map selection, lane 10 RT shadow rays.
//
// Units:
//   direction   — unit vector, world space.  Conventionally points FROM the
//                 light source TOWARDS the origin (i.e. the reverse of the
//                 traditional "light position" convention used in some engines).
//                 Normalise before writing.
//   color       — linear sRGB [0, 1]
//   intensity   — illuminance at the surface perpendicular to the light,
//                 lux [lx = lm/m²]  (photometric)
//   cast_rt_shadow — when true, lane 10 traces hard-shadow rays from this
//                 source; when false, lane 09 falls back to CSM.

PSYNDER_COMPONENT(LightDirectional) {
    psynder::math::Vec3 direction;       ///< unit vector FROM light TOWARD scene
    f32                 intensity;       ///< illuminance, lux [lx]
    psynder::math::Vec3 color;           ///< linear sRGB, [0, 1]
    u32                 cast_rt_shadow;  ///< bool as u32 for POD alignment
};
static_assert(sizeof(LightDirectional) == 32, "LightDirectional layout frozen at 32 bytes");

// ─── CameraComponent ─────────────────────────────────────────────────────────
//
// Camera attached to an entity.  Lane 09 builds a View from this each frame;
// there may be multiple active cameras (mirrors, split-screen, shadow probes).
//
// Lifecycle:
//   Written by: the player controller / editor camera system.
//   Read by:    lane 09 (primary/secondary render views), lane 10 (view
//     frustum for RT primary rays), lane 11 (viewport for post-processing).
//
// Units:
//   view_mtx     — right-handed world-to-camera transform (column-major)
//   proj_mtx     — row-major NDC projection (RH perspective, standard 0/1 depth
//                  range; lane 09 owns the reverse-Z promotion at M3)
//   viewport_x/y — top-left corner, pixels
//   viewport_w/h — width / height, pixels
//   near_z/far_z — near and far clip planes, metres

PSYNDER_COMPONENT(CameraComponent) {
    alignas(16) psynder::math::Mat4 view_mtx; ///< world-to-camera (V), RH
    alignas(16) psynder::math::Mat4 proj_mtx; ///< projection (P), standard depth
    u32 viewport_x;   ///< viewport top-left X, pixels
    u32 viewport_y;   ///< viewport top-left Y, pixels
    u32 viewport_w;   ///< viewport width, pixels
    u32 viewport_h;   ///< viewport height, pixels
    f32 near_z;       ///< near clip plane, metres  (> 0)
    f32 far_z;        ///< far  clip plane, metres  (> near_z)
    f32 fov_y_rad;    ///< vertical field-of-view, radians
    f32 aspect;       ///< width / height
};
static_assert(sizeof(CameraComponent) == 160, "CameraComponent layout frozen at 160 bytes");

// ─── VisibleBit ──────────────────────────────────────────────────────────────
//
// Tag-style partition marker.  Every renderable entity carries exactly one
// VisibleBit to identify its partition bucket in the ECS archetype graph.
// Lane 09 uses this to run separate queries for static vs dynamic buckets
// and choose the appropriate GPU-cull path.
//
// Lifecycle:
//   Written by: spawner / level-load (WorldStatic never changes; WorldDynamic
//     may be toggled if an entity is "frozen" — physics sleep, etc.).
//   Read by:    lane 09 GPU-cull compute (selects static BLAS vs dynamic BLAS
//     for RT), lane 06 scene-graph dirty propagation (WorldStatic skips the
//     transform-propagation sweep unless explicitly dirtied).
//
// Partition enum values mirror DESIGN §9.4 (WorldStatic / WorldDynamic /
// Effects / UI).  Only WorldStatic and WorldDynamic are ECS render targets;
// Effects and UI have separate draw lists managed by their respective lanes.

PSYNDER_COMPONENT(VisibleBit) {
    enum class Partition : u8 {
        WorldStatic  = 0, ///< geometry baked into static BLAS; no per-frame update
        WorldDynamic = 1, ///< geometry in dynamic BLAS; TransformWS updates each frame
        Effects      = 2, ///< particles / decals — separate render list (lane 11)
        UI           = 3, ///< screen-space — render list owned by lane 21
    };

    Partition partition;
    u8        _pad0;
    u16       _pad1;
};
static_assert(sizeof(VisibleBit) == 4, "VisibleBit layout frozen at 4 bytes");

}  // namespace psynder::scene
