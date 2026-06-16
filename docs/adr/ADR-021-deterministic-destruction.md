# ADR-021: Deterministic destruction — precomputed fracture, Jolt chunks, replayable

- **Status:** Proposed (stub — to be expanded before lane 17-physics-destruction starts)
- **Date:** 2026-05-28
- **Related:** ADR-019 (Jolt for authoritative rigid bodies; ECS-authoritative `scene::World`), ADR-018 (deterministic command buffers), ADR-020 (replication of dynamic bodies). Builds on Issue #1 (`DestructionWorldState` over `OpaqueWorldState`).

## Context

Large-scale destruction is Battlefield/Frostbite's signature. The opportunity here is to do it **deterministically** — Frostbite's destruction is non-deterministic eye-candy that can't exist in a server-authoritative lockstep world. If our fracture is bit-exact reproducible, destruction is fully authoritative, replayable in demos, and consistent across all clients. Constraints: the lockstep determinism pillar, the DOTS contract, real metric masses, and a frame/perf budget (destruction must not blow the tick).

## Decision (direction)

- **Offline fracture authoring.** Bake convex-shard fracture patterns (Voronoi/clustered) per destructible mesh at cook time — *not* realtime boolean/CSG. Shards are convex hulls Jolt can simulate cheaply.
- **Deterministic activation.** On a damage threshold (from the authoritative hit/explosion event), spawn the precomputed shards as **Jolt rigid bodies** (mass from material density) seeded by a **deterministic per-event RNG** (tick + stable entity id), synced back to ECS `TransformWS` each tick like any rigid body (ADR-019 class 2).
- **WorldState seam.** Route through the `DestructionWorldState` concretization of `OpaqueWorldState` (Issue #1) so destruction state is replicated/serialized through one typed boundary.
- **Budget & lifecycle.** Pooled shard bodies, island sleep + merge, distance/age-based despawn, a per-tick spawn cap, and an LOD that converts settled debris back to static or removes it.

## Alternatives rejected

- **Realtime CSG/boolean fracture.** Expensive and non-deterministic across platforms — fails the #1 pillar.
- **Voxel/FEM destruction.** Memory- and compute-heavy; overkill for shard-based competitive maps.
- **Cosmetic-only (non-authoritative) destruction.** Rejected as the *primary* model — it desyncs gameplay-relevant cover; cosmetic debris may still ride a separate non-authoritative path.

## Consequences

- Destruction reuses the Jolt rigid-body pipeline (ADR-019 class 2) — that pipeline is a prerequisite.
- Replicating many short-lived dynamic bodies stresses ADR-020's snapshot priority/aging — destruction is a key test case for it.
- Static colliders that get destroyed must drop their projected Jolt static body — the same lifecycle just added (`remove_static_body`) on `character_spine`.

## Status of implementation

- ⬜ Stub. Depends on Jolt rigid-body enemies (ADR-019 class 2) and Issue #1. Expand with the fracture-bake format, damage model, and replication/perf budget before coding.

## References

- DESIGN-PSYNDER-GX.md §13 (milestone ordering — destruction is a later milestone), ADR-019, ADR-018, Issue #1.
- Jolt Physics convex shapes + islands; Voronoi pre-fracture (e.g. Houdini/NvBlast-style baked patterns, deterministic playback).
