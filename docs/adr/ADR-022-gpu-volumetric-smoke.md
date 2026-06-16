# ADR-022: GPU volumetric smoke — compute-driven, cross-API, gameplay-fair

- **Status:** Proposed (stub — to be expanded before lane 08-shader starts)
- **Date:** 2026-05-28
- **Related:** ADR-019 (ECS-authoritative sim), the `psy::gpu` Vulkan+Metal abstraction (lane 07), render pipeline (lane 09) / post (lane 11), ADR-020 (authoritative state). Lane ownership: 08-shader + 11-render-post, querying lane 07 for compute.

## Context

CS2's dynamic, lightable, bullet-displaceable **volumetric smoke** is the standout modern-FPS feature, and it doubles as gameplay (it blocks line-of-sight). It also lands squarely on Psynder-GX's identity: a **GPU-accelerated** engine with a unified **Vulkan + native Metal** backend. The opportunity to be *better*: make the smoke's line-of-sight occlusion **deterministic and identical on every client**, so what you see and what the server rules are the same — fairness CS2 approximates server-side.

## Decision (direction)

- **Density field.** A sparse 3D voxel grid (brick/clipmap around active volumes) holding smoke density, filled and evolved by a **compute shader**: grenade emitters inject density; advection + dissipation evolve it; world geometry bounds expansion (smoke fills rooms/conforms to cover).
- **Displacement.** Bullets/explosions inject impulse that **carves transient holes** (peek windows) into the field.
- **Rendering.** Raymarch the field in a post pass with single/multi-scatter lighting and depth-aware compositing; froxel/temporal upsample for cost.
- **Cross-API once.** Author through `psy::gpu` compute + `psynder::shader` so a single implementation runs on Metal and Vulkan (no `vk*`/`MTL*` in lane code — AGENTS.md).
- **Gameplay fairness (the differentiator).** Compute the *authoritative* sight-blocking on a **coarse, deterministic** density grid stepped on the fixed tick (CPU or determinism-safe compute), shared with gameplay LOS — the high-res GPU volume is the *visual* of that same authoritative state. No per-frame heap alloc; pooled GPU buffers (no `newBuffer*` in the frame loop — AGENTS.md §4.4).

## Alternatives rejected

- **Billboard/particle smoke.** Cheap but flat, doesn't conform to geometry, and can't give consistent LOS — gameplay-unfair.
- **CPU fluid sim.** Too slow for interactive resolution; wastes the engine's GPU identity.
- **Fully GPU-authoritative LOS.** Cross-vendor GPU FP is non-deterministic — unusable on the lockstep path (same reasoning as ADR-019's GPU-agent-sim rejection); hence the split authoritative-coarse / visual-fine design.

## Consequences

- Needs a `psy::gpu` compute path + 3D/sparse texture support — file against lane 07 if missing.
- Establishes the froxel volumetrics infrastructure reusable for fog/god-rays later.
- Adds a deterministic coarse-grid sim tick that gameplay LOS queries.

## Status of implementation

- ⬜ Stub. Expand with grid layout, the compute pass graph, the authoritative-vs-visual split, and the perf budget before coding.

## References

- DESIGN-PSYNDER-GX.md §13, ADR-019 (determinism / GPU rejection rationale), AGENTS.md (Vulkan/Metal symmetry, allocator rules).
- CS2 volumetric smoke; froxel/volumetric fog (Frostbite "Physically Based Unified Volumetric Rendering").
