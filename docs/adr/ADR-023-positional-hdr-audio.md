# ADR-023: Positional HDR audio — occlusion, HRTF, tick-deterministic mix

- **Status:** Proposed (stub — to be expanded before lane 14-audio starts)
- **Date:** 2026-05-28
- **Related:** ADR-019 (physics world for geometric queries), ADR-018 (deterministic tick / event ordering), ADR-020 (replicated/authoritative audio events). Lane ownership: 14-audio, querying lane 15-physics-core for occlusion rays.

## Context

Competitive FPS is won by the ears: footstep direction, gunfire bearing, reload tells. CS:GO and Frostbite both invest heavily here (Frostbite's **HDR audio** mix is a signature). The "better" angle for Psynder-GX: tie audio-event scheduling to the **fixed deterministic tick** so the mix is **reproducible in replays/demos** (debuggable, anti-cheat-reviewable) — something amplitude-panned engines can't promise. Constraints: no per-frame heap alloc in the mixer (DESIGN §4.4 / AGENTS.md), cross-platform, low latency.

## Decision (direction)

- **HDR mixing.** Perceptual-loudness windowing so loud transients (gunshots, explosions) dynamically duck the noise floor and quiet sounds don't mask the audible field — a wide effective dynamic range without clipping.
- **Geometric occlusion/obstruction.** Cast rays against the **physics world** (the same Jolt/ECS colliders gameplay uses) from listener to source; apply attenuation + low-pass for walls (occlusion) and partial paths (obstruction); optional portal/path propagation later.
- **Spatialization.** HRTF binaural panning for headphones (the competitive default), with distance attenuation + air absorption; speaker-panning fallback.
- **Determinism.** Audio events are emitted by the sim as components/queue entries (ADR-018) stamped with tick + stable entity id; the mixer consumes them in a stable order so a replay reproduces the same mix. The DSP graph is **pooled** (no alloc in the audio callback).
- **Optional GPU assist.** Convolution reverb / large filter banks may offload to `psy::gpu` compute if profiling justifies (non-authoritative — purely the rendered mix).

## Alternatives rejected

- **Amplitude-only stereo panning.** Poor directionality; inadequate for competitive play.
- **Full wave-based acoustic simulation.** Far too costly per-frame at FPS budgets; ray-based occlusion + HRTF is the right cost/quality point.
- **Non-deterministic event scheduling (wall-clock driven).** Breaks replay reproducibility — events must ride the tick.

## Consequences

- Audio takes a read-only dependency on the physics broadphase/colliders for occlusion rays (coordinate with lane 15).
- Establishes the pooled DSP-graph + HRTF infrastructure; reverb/propagation are later refinements.
- The replay/demo system (scene lane) gains bit-reproducible audio.

## Status of implementation

- ⬜ Stub. Expand with the DSP-graph design, HRTF dataset choice, occlusion-ray budget, and the audio-event component schema before coding.

## References

- DESIGN-PSYNDER-GX.md §4.4 (no frame-loop alloc), ADR-018 (deterministic events), ADR-019 (physics world).
- Frostbite HDR audio; Steam Audio / Oculus Audio HRTF + occlusion; CS:GO positional-audio model.
