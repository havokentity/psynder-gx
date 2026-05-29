# Psynder-GX — Autonomous Roadmap & Journal

> Living plan the autonomous loop drives off. Read with `AUTONOMOUS-CHARTER.md`.
> Each iteration: pick the top unblocked item, build it green, land it, then
> append a journal entry. Keep this file honest and current.

- **Trunk:** `nextgen/new-release`
- **Budget:** ~12 h autonomous. Started: 2026-05-30 03:51 IST (target ~12h).
- **Definition of Done:** see Charter §8.

## Current state (entering autonomous mode)

Engine has: DOTS `scene::World` ECS (stable hashed component ids); Vulkan+Metal
`psy::gpu`; render pipeline + Extract; Jolt hybrid physics (player capsule,
dynamic props w/ knockback, fracture/destruction shards); **DOTS mass-agents**
(steering + hard static non-penetration + planar capsules); netcode primitives
(snapshot delta, prediction/reconcile, lag-comp, AoI-on-broadphase) **+ a
deterministic server↔client `ReplicationSession` loop**; deterministic input
replay; golden determinism gate; jobs; world/bsp PVS scaffold; audio mix
scaffold; `02_crate` playable (FPS controller, shooting/fracture, dynamic crates,
steering crowd, `debugdraw` collider/flow overlays incl. wireframe). CI green on
macOS/Metal + Linux/Vulkan + Windows + determinism matrix. 583 unit tests green.

## Prioritized backlog (top = next)

### N — Netcode (finish the loop into real play)
- [x] Wire lag-comp hitbox rewind into `ReplicationSession` (rewind to client
      view-time for server-side hitreg); golden test. **DONE (iter 1).**
- [x] Per-peer interest management in the session (use `InterestSet` broadphase);
      bandwidth + correctness tests. **DONE (iter 2).**
- [ ] Over-the-wire integration: drive the session through `Loopback`/`HostImpl`
      transport (Input + snapshot serialization), an end-to-end loopback test.
- [ ] Client/server split in the player path (server ticks authoritative; client
      predicts) — headless server smoke.

### G — Gameplay slice (FPS)
- [ ] Weapons: hitscan + projectile, fire rate, spread; damage application via
      lag-comp rewind; tests.
- [ ] Health/armor/damage/death/respawn components + systems (deterministic).
- [ ] Rounds/scoring/spawn points; pickups (health/ammo/weapon).
- [ ] FPS controller polish (air control, crouch/jump tuning) on the Jolt capsule.

### A — DOTS agents / AI
- [ ] Navmesh or flow-field pathing on the spatial broadphase (research first).
- [ ] Combat bots that path + shoot through the gameplay systems; scale test to
      thousands; golden determinism.

### M — Maps / world
- [ ] Quake3-class arena: BSP/brush geometry + PVS cull wired into render +
      collision; a `samples/arena` demo.
- [ ] BF-light larger outdoor map (heightfield/streaming groundwork).

### R — Rendering
- [ ] Adopt `render::pipeline::render()` in the player (retire the bespoke
      `scene_pipeline`); keep determinism of extract.
- [ ] PBR-ish lighting + post (tonemap/bloom). Wireframe fill mode (done) reused.
- [ ] **LAST:** minimal RT reflections + shadows behind a build flag.

### E — Editor (WYSIWYG)
- [ ] ECS scene authoring (retire `ScenePrimitive`); save/load loose scene.
- [ ] Transform gizmos + entity inspector + asset browser; play-in-editor.
- [ ] PsyGraph node editor; live IPC to the running player.

### S — Scripting (PsyGraph)
- [ ] Behavior IR → DOTS compiler growth (ADR-018); more nodes; hot reload;
      drive a gameplay behavior from a graph.

### D — Demos (test harness while editor matures)
- [ ] `samples/arena` (Quake3), crowd-combat, destruction sandbox, vehicle test,
      net-play headless demo — one per milestone as needed.

### P — Perf & determinism
- [ ] Cross-platform golden parity: strict-FP the sim path (scene/math used by
      agents+physics) so Linux/Windows match macOS; flip determinism.yml off
      continue-on-error where it holds.
- [ ] Alloc audits (no per-frame heap in hot paths); SIMD/job scaling; profiling.

## Journal

> Append one entry per iteration: what shipped, decisions/assumptions, sources,
> follow-ups, anything needing eventual human/in-window/PC-Vulkan check.

- (init) Charter + roadmap created; entering autonomous mode at 583 green tests,
  CI green. First item: **N — lag-comp hitbox rewind into the session.**
- (iter 1) **Lag-compensated server-side hitreg in `ReplicationSession`.** Added
  a per-tick authoritative snapshot-history ring (window > 2*latency) + a
  deterministic `raycast_nearest_entity` (ray-vs-sphere, no trig). A `kInputBtnFire`
  input rewinds the world to the shooter's view-tick (server_tick - 2*latency)
  and ray-tests there, recording `HitEvent{attacker,victim,tick}`. Tests: shot
  hits the target where the shooter SAW it while a naive current-world ray misses
  (proves lag comp), + a raycast unit test. 585/585 green. Decision: the net
  session uses its own EntityState snapshot-history rewind (cleaner than
  shoehorning LagComp.h's physics-oriented OpaqueWorldState); LagComp.h remains
  the physics-side rewind. Caveat: fire direction uses libm sin/cos (per-platform
  deterministic, not yet cross-platform bit-identical) — fine for hitreg, flagged
  for the cross-platform determinism pass (P). Next: **N — per-peer interest
  management in the session.** CI verification of this push happens at the start
  of iter 2.
- (iter 2) **Per-peer area-of-interest in `ReplicationSession`.** Verified iter-1
  CI green first. Added an `aoi_radius_m` (default infinite). Finite radius
  engages the `InterestSet` broadphase (rebuilt from the authoritative world each
  tick, queried per peer around its own entity); each client then receives a full
  snapshot of only the entities in its interest sphere (own always included).
  Default (all-visible) keeps the existing shared-delta compression untouched, so
  the keystone convergence/lag-comp/determinism tests are unchanged. Tests: a
  distant peer is culled at a 5 m radius (visible_count==1) while the server keeps
  the full world; infinite radius keeps all visible. 588/588 green. Decision: AoI
  snapshots are full-per-peer (membership exact as entities enter/leave); the
  delta codec (apply_delta overlays, no removals) is unchanged — inter-frame AoI
  delta WITH removals is a tracked follow-up (would need a removed-ids section in
  the wire format + the size-pinned snapshot tests updated). Next: **N —
  over-the-wire transport integration (Loopback/HostImpl).**
