# ADR-018: Behavior IR → DOTS compiler (the engine's execution model)

- **Status:** Accepted
- **Date:** 2026-05-27
- **Supersedes:** ADR-GX-016 (shared Lua `psy-script` module) for the gameplay-authoring path. Lua remains only as a transitional editor-preview backend during migration.

## Context

Four product pillars constrain the gameplay layer:

1. **Author whole games from the editor** — Delta-Force / Quake-class shooters — with no engine recompile.
2. **Always cache-coherent, archetype ECS / DOTS.** No exceptions.
3. **High performance, no garbage:** everything pooled, no per-frame heap allocation, no GC.
4. **No-code authoring, but C++ is a first-class option too.**

A traditional embedded scripting VM (the original ADR-GX-016 Lua decision, and the short-lived custom `.psy` bytecode interpreter that was prototyped and then removed) is fundamentally at odds with pillars 2 and 3: a per-op interpreter dispatching on scalar values is the opposite of cache-coherent bulk SIMD, and it allocates. We are also **multiplayer-ready from the first commit** (server-authoritative, 128-tick-capable, lockstep replay — ADR-GX-005), which makes **bit-determinism** a hard requirement on anything that touches simulation.

The realization that resolves the tension: **the end user architects a *behavior*; they never reason about chunks, archetypes, or SoA. The compiler is the only component that knows about DOTS, and it *always* emits DOTS.** Heterogeneous-looking per-entity logic is heterogeneous *within* one entity's update but **homogeneous across all entities** in a chunk (same archetype + same behavior ⇒ identical op sequence, differing only in data). That cross-entity homogeneity is the lever that makes everything vectorizable.

## Decision

Build a **Behavior IR → DOTS compiler**. One intermediate representation, two front-ends (visual graph for designers, hand-written C++ for programmers), one DOTS backend. The compiler performs two core transformations:

### 1. Transpose (loop fission): entity-major → op-major

Authored intent is entity-scalar ("for this projectile: `vel += g·dt; pos += vel·dt; if grounded → impact`"). The compiler rewrites it to op-major passes over a chunk — "for each op, all entities" — so every pass is homogeneous and SIMD-perfect. Intermediates between passes live in chunk-sized scratch streams that stay cache-resident (the reason archetype chunks are ~16 KB / ~1024 elements). The arithmetic core is recorded **once** into a `psynder::math::MathLogicKernel` program, cached, and executed with pointer-swaps each frame (zero per-frame allocation — pillar 3).

### 2. Classify every op into one of three buckets

| Bucket | Contents | Execution |
|---|---|---|
| **Arithmetic core** | pure, branch-free math on component columns | recorded into the math kernel → SIMD passes over the chunk |
| **Branch** | data-dependent control flow | lowered to a **mask** (compute-both-select) or a **partition/compaction** pass; expensive/rare branches use enableable-component chunk filtering |
| **Effect** | spawn, destroy, add/remove component, events, RPC | appended to a **pooled command buffer**, replayed deterministically at the next sync point — never mutates structure inline |

Component **read/write sets are inferred** from the ops (the user declares nothing). Those sets drive (a) the archetype query and (b) the job scheduler, which runs systems with disjoint write-sets in parallel across chunks (thread parallelism) while each chunk runs SIMD internally (lane parallelism). The two parallelism axes are exploited automatically.

### Pipeline

```
authoring (visual graph OR hand C++)
  → Behavior IR                         entity-scalar; how the user thinks
  → analysis
      • component read/write inference   → archetype query + race-free schedule
      • effect classification            → split into core / branch / effect
      • loop fission                     → entity-major becomes op-major
  → lowering
      • core    → MathLogicKernel op-program (compiled once, cached)
      • branch  → mask / compaction passes
      • effect  → command-buffer emits
  → codegen: C++ system struct (psynder::generated::*) — AOT for shipping,
             dlopen hot-reload for the editor
  → schedule: disjoint write-sets parallel across chunks; SIMD within a chunk
```

### Determinism (multiplayer, from the start)

- Fixed, compiler-assigned pass order.
- `-fno-fast-math` on every generated translation unit (consistent with the physics/netcode TU policy).
- **Command buffers replay in a deterministic order — sorted by stable entity id, never by worker-completion order.**
- No nondeterministic iteration: chunk traversal order is stable; structural changes are batched at explicit sync points.

## Consequences

- **Removed:** the custom `.psy` bytecode VM (`PsyCompiler`/`PsyVm`) — a runtime interpreter is the opposite of compile-to-C++. Its only test was repurposed to the visual-graph path.
- **Kept and extended:** `engine/script/internal/VisualGraphCompiler.cpp` already emits C++ (`namespace psynder::generated::psygraph`) alongside Lua and a debuggable PsyScript IR. That C++ emitter is the seed of this backend.
- **Lua becomes transitional** — retained only as the live editor-preview VM until the C++ codegen + hot-reload path is in place, then removed lane-by-lane behind a flag. DESIGN §10.5 must be updated.
- **The authoring surface hides DOTS; codegen enforces it.** Designers compose behaviors; the compiler guarantees cache-coherent, race-free, deterministic output. For the hand-C++ path the guarantee is cultural (API shape + lint + profiling), not compiler-enforced.
- A frame becomes a **phased pipeline** (input → simulate → resolve → structural-change barrier → render-extract); events are components/queues one phase writes and the next reads.
- The math kernel is the per-system SIMD execution engine; chunk tiling keeps intermediates in cache.

### Known hard problems (designed for, not deferred)

1. **Branch divergence** — masking wastes lanes when rare; partitioning costs a compaction. Start with masking; add enableable-component filtering for the expensive cases.
2. **Cross-entity reads** (targeting, flocking) break chunk-locality. The IR must detect "reads another entity" and route to a **gather pass against a read-only double-buffered snapshot**, never a live random-access read.
3. **Frame phasing & structural-change barriers** must be explicit so deferred effects never invalidate in-flight iteration.
4. **Determinism** must be proven, not assumed — golden replay tests across platforms.

## Validation

A thin proving spine ships alongside this ADR (`engine/script/behavior/`, `tests/unit/script_behavior_spine.cpp`): a hand-lowered "projectile" behavior — integrate core (two fused kernel passes) + a ground-hit mask + a deferred-destroy command buffer — executed op-major over an SoA chunk of N entities, verified bit-equal to a scalar per-entity reference and allocation-free in steady state. It proves the transpose, the three buckets in miniature, and the no-alloc/determinism claims before the full front-ends are built.

## References

- DESIGN-PSYNDER-GX.md §3 (DOTS contract), §10.4 (netcode / sub-tick determinism), §10.5 (scripting — to be revised), §10.8 (in-engine editor)
- ADR-GX-005 (server-authoritative, lockstep replay), ADR-GX-016 (superseded), ADR-004 (in-engine editor)
- Unity DOTS (archetype chunks, IJobChunk), data-oriented design (Acton)
