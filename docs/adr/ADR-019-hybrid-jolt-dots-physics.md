# ADR-019: Hybrid physics — Jolt for dynamics, DOTS for mass agents, ECS-authoritative

- **Status:** Accepted
- **Date:** 2026-05-28
- **Related:** ADR-018 (DOTS execution model), ADR-GX-005 (server-authoritative lockstep), the scene-on-ECS migration (`scene::World` is the single source of truth).

## Context

The engine needs collision/physics for several very different workloads, under two hard constraints from the product pillars: **cross-platform bit-determinism** (server-authoritative, 128-tick-capable lockstep — ADR-GX-005) and **always cache-coherent DOTS** (ADR-018). The workloads:

1. **The player** — one capsule character that needs precise feel: step-up, slopes, stable penetration recovery.
2. **"Full physics" enemies / props** — pushable, stackable, ragdoll-capable, react to forces. Heterogeneous, arbitrary convex shapes, rotational dynamics, resting contacts.
3. **Mass agents** — potentially thousands of homogeneous steering/navmesh enemies that need cheap ground/wall collision + local avoidance, not full rigid-body dynamics.

A single answer is wrong for all three. The analysis that settles it:

- **General rigid-body dynamics** (workloads 1–2: arbitrary convex + rotation + stacking + determinism) is a deep specialty. Jolt is ~a decade of focused work, shipped in a AAA title, and — uniquely among the realistic options — is **designed for cross-platform determinism** (`CROSS_PLATFORM_DETERMINISTIC=ON`, already wired with `-fno-fast-math -ffp-contract=off`, AVX capped to AVX2). Hand-rolling this to comparable robustness *and* re-proving determinism is a multi-person-year, high-risk effort with negative ROI when Jolt is free (MIT), already integrated, and already determinism-tuned. A short-lived hand-rolled AABB capsule resolver confirmed this the hard way (no broadphase, conservative-AABB artifacts, pass-through bugs).
- **Mass homogeneous agents** (workload 3) are the *opposite* case: uniform shapes, simple math, embarrassingly parallel. This is where a bespoke DOTS system genuinely beats a general engine — SoA columns, a spatial broadphase (`engine/scene/UniformGrid`/`Bvh`), SIMD, parallel chunks, no ECS↔engine sync. The win is real **only** if it's built well (broadphase + SoA), which a general engine's per-body generality can't match at that scale.

### Alternatives rejected

- **PhysX 5.** Cannot guarantee cross-platform determinism (and its GPU path is non-deterministic by design); its GPU acceleration is CUDA — **NVIDIA-only**, unusable on this engine's Vulkan + **Metal/Apple-Silicon** targets; macOS support was dropped. Its strengths are exactly the things this engine can't use; its weakness (determinism) is this engine's #1 pillar.
- **A fully custom DOTS rigid-body engine** (for workloads 1–2). The hard parts — GJK/EPA convex collision, contact manifolds, stable stacking, CCD, joints/articulation for ragdolls, deterministic parallel solving across platforms — are years of specialist work. Bad ROI versus Jolt. Reconsider only if a profiling-proven bottleneck or a GPU-compute physics moonshot justifies it, behind its own ADR.

## Decision

A **hybrid**, with **`scene::World` as the single authoritative scene**. Every "mover" reads input/forces and writes `TransformWS`; nothing else owns a parallel *mutable* scene.

| Actor | Solver |
|---|---|
| Player | **Jolt** `CharacterVirtual` (via `character_spine`) |
| Full-physics enemies / props (rigid, ragdoll, convex, authoritative) | **Jolt** rigid bodies |
| Mass steering/navmesh agents | **Custom DOTS** movement: steering + local avoidance + cheap capsule-vs-static on `UniformGrid`/`Bvh` |

**Boundary / glue:**
- Static `Collider` entities are **projected** from the ECS into Jolt at play-start (`EcsCharacterBridge::build_jolt_statics_from_ecs`), with rotation recovered from `TransformWS` (`rotation_from_transform`). Statics are immutable for the session, so this is a derived projection, not a parallel mutable world.
- The Jolt-solved player/enemy transforms are **written back** into the ECS `TransformWS` each tick. Rendering, gameplay, and netcode read only the ECS.
- Mass agents query **one spatial index** rebuilt each tick from ECS `TransformWS` (statics + physics enemies + agents) so they avoid each other, the player, and Jolt bodies without being full physics; they also write `TransformWS`.

**Determinism contract (lockstep):**
- Fixed tick; a fixed, documented **system order** each tick (e.g. Jolt step → publish positions → DOTS agents read last-published positions → agents write).
- Jolt in cross-platform-deterministic mode; DOTS agent kernels under `psynder_determinism_fp` (`-fno-fast-math -ffp-contract=off`), stable iteration order (note `World` swap-remove makes intra-chunk order history-dependent — systems must not depend on it).
- Proven, not assumed: a **golden cross-platform replay test** gates the determinism pillar (tracked separately; see ARCH review A11).

## Status of implementation

- ✅ **Player on Jolt via ECS** — `engine/physics/core/EcsCharacterBridge.h` + `samples/02_crate` play mode. The hand-rolled AABB resolver was removed; `CharacterController.h` is now the player's ECS representation (state written back from the solver). Tests: `tests/unit/physics_character_controller.cpp` (settles on ground, blocked by an ECS-projected wall, rotation extraction).
- ⬜ **Full-physics enemies** — Jolt rigid-body capsules spawned from ECS, synced back. Reuses the same Jolt world.
- ✅ **DOTS mass-agent system** — `engine/physics/agents/` (lane `physics-agents`). Homogeneous steering agents (seek + Reynolds-arrive, local separation, capsule-vs-static push-out) on the scene `SpatialIndex` broadphase, parallelised via `JobSystem::parallel_for`. Agents are ECS entities in `scene::World` (`Agent`/`AgentVelocity`/`AgentTarget` + `TransformWS`) — no parallel mutable store. The tick reduces avoidance accel serially in stable entity-id order, then integrates+writes in parallel over an immutable snapshot, so it is bit-reproducible across thread counts. Golden determinism test: `tests/unit/agents_determinism.cpp`.

## Consequences

- We rent the hard, solved problem (general rigid-body dynamics + determinism) from Jolt and own the narrow, high-scale problem (homogeneous agents) where DOTS pays off.
- One ECS-authoritative boundary means movers are swappable: the player's mover, a future DOTS character controller, or a GPU-compute agent sim are drop-ins, not rewrites.
- **Future moonshot (not adopted):** a GPU-compute agent/particle sim fits the engine's GPU-accelerated identity and could exceed any CPU solver for cosmetic or non-lockstep swarms — but cross-vendor GPU FP determinism makes it unsuitable for the authoritative lockstep path. Revisit behind its own ADR if a cosmetic/non-authoritative use case appears.
