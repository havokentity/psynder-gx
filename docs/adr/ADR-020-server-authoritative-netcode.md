# ADR-020: Server-authoritative netcode — snapshot replication, prediction, lag compensation

- **Status:** Proposed (stub — to be expanded before lane 18-net starts)
- **Date:** 2026-05-28
- **Related:** ADR-GX-005 / DESIGN §10.4 (server-authoritative, 128-tick lockstep, sub-tick determinism), ADR-018 (DOTS execution model — deterministic command buffers), ADR-019 (Jolt player/rigid bodies; ECS-authoritative `scene::World`).

## Context

Competitive FPS netcode (CS:GO/Source 2, Battlefield/Frostbite) is the product's spine: **server-authoritative**, cheat-resistant, 128-tick-capable, and good under real-world latency and loss. Two pillars shape every choice here:

- **Bit-determinism** (the sim already targets it — ADR-018/019). This is the lever that makes our netcode *better than* the references: a deterministic mover means client prediction replays **bit-exact**, and a deterministic sim means a demo is just the input stream.
- **DOTS / SoA ECS** (`scene::World` is authoritative). State to replicate already lives in contiguous component columns — delta compression is column-wise, not per-object dirty-bit bookkeeping.

## Decision (direction)

Build server-authoritative netcode on the existing ECS + fixed tick, in four layers (each its own lane-18 sub-stream / Issue):

| Layer | Mechanism | "Better than" |
|---|---|---|
| **Replication** | Baseline + **delta-compressed snapshots** off ECS SoA columns; reliable (events/RPC) + unreliable (state) channels over UDP; per-entity priority/aging. | Column-wise delta over archetype chunks is cheaper and cache-coherent vs. per-object dirty tracking. |
| **Client prediction + reconciliation** | Client predicts the local pawn (Jolt `CharacterVirtual`, deterministic) and **replays unacknowledged inputs** on server correction. | Deterministic mover ⇒ prediction is bit-exact ⇒ no rubber-banding under stable latency. |
| **Lag compensation** | Server keeps a rolling **per-tick hitbox-transform history ring**; rewinds to the shooter's perceived time for hitscan/projectile resolution. | Rewind is deterministic and replayable, not a heuristic. |
| **Interest management (AoI)** | Per-client relevancy set from the **shared `UniformGrid`/`Bvh` broadphase** (same index used by DOTS agent avoidance + physics queries). | One spatial structure for AoI, steering, and queries; bandwidth scales with local density, not world size. |

**Determinism contract additions:** fixed tick; snapshots/inputs stamped with tick (and, as a later refinement, **sub-tick input timestamps** à la CS2); command buffers replay sorted by stable entity id (per ADR-018); `-fno-fast-math` on all net TUs (per AGENTS.md).

## Alternatives rejected

- **Client-authoritative / trust-the-client.** Non-starter for a competitive title — it is cheating-by-design.
- **Full-state every tick (no delta).** Bandwidth-prohibitive at 128 tick / BF player counts.
- **Rollback-netcode (GGPO-style) as the primary model.** Fits 2-player fighting games, not 64–128-player server-authoritative shooters; we use server-auth + client prediction instead (rollback techniques may inform local reconciliation).

## Consequences

- The golden cross-platform replay test (ARCH-A11) is a hard gate — prediction and lag-comp are only correct if the sim is provably deterministic. **Pin Jolt by SHA (ARCH-B3) is a prerequisite.**
- Demos/replays (a separate scene-lane item) become input-only + periodic state keyframes, thanks to determinism.
- Hitbox history (lag-comp) needs a transform-history ring in/adjacent to lane 15-physics-core.

## Status of implementation

- ⬜ Stub. Sub-divide lane 18-net into replication-core / prediction / relevancy; lag-comp history lands with lane 15. Expand this ADR with wire format, channel design, and the bandwidth/serialization budget before coding.

## References

- DESIGN-PSYNDER-GX.md §10.4 (netcode / sub-tick determinism), ADR-018, ADR-019, ADR-GX-005.
- Valve "Source Multiplayer Networking" + CS2 sub-tick; Glenn Fiedler "Networked Physics"; Frostbite interest management talks.
