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
- [x] Over-the-wire integration: drive the session through `Loopback`/`HostImpl`
      transport (Input + snapshot serialization), an end-to-end loopback test.
      **DONE (iter 3).**
- [ ] Client/server split in the player path (server ticks authoritative; client
      predicts) — headless server smoke.

### G — Gameplay slice (FPS)
- [x] Weapons: hitscan + projectile, fire rate, ammo; damage application; tests.
      **DONE (iter 5).** (Spread + wiring net HitEvents to apply_damage on the
      server lag-comp path = follow-ups with the client/server split.)
- [x] Health/armor/damage/death/respawn components + systems (deterministic).
      **DONE (iter 4) — new `engine/gameplay` lane.**
- [~] Rounds/scoring/spawn points; pickups (health/ammo/weapon). **Scoring +
      pickups DONE (iter 6); rounds/spawn-point selection = follow-up.**
- [ ] FPS controller polish (air control, crouch/jump tuning) on the Jolt capsule.

### A — DOTS agents / AI
- [x] Navmesh or flow-field pathing (research first). **DONE (iter 7) —
      deterministic grid flow-field in the new `engine/ai` lane.**
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
- (iter 3) **Over-the-wire replication through the real rUDP transport.** New
  test `net_over_the_wire.cpp` drives the netcode payloads through `HostImpl` over
  the `LoopbackBus` (not the in-memory channel): (a) an `Input` and a snapshot
  `encode_delta` cross the wire byte-intact (reply via `InboundMessage.from`);
  (b) a 20-input move replicates end-to-end — client sends inputs over the wire,
  server integrates with the shared `step_entity` and ships snapshots, and the
  client's view converges BIT-EXACTLY to a reference integration. Uses the
  `make_test_host`/`pump` harness; reliable delivery flushed by pumping the rUDP
  window. 590/590 green. Decision: pinned the serialization+transport path the
  session plugs into; a pluggable transport adapter for ReplicationSession is the
  tracked follow-up (keeps the deterministic in-memory channel as the unit-test
  default). Note: Input/snapshot use POD memcpy — endianness is a cross-platform
  follow-up (consistent with the existing codec). Next: **N — client/server split
  in the player path.**
- (iter 4) **FPS gameplay foundation: health/armor/damage/death/respawn** — new
  `engine/gameplay` lane (`psynder_gameplay`, determinism FP flags; auto-covered
  by perf_guardrails). POD components Health/Armor/Dead/Respawnable; deterministic
  `apply_damage` (Quake armor ratio 2/3, capped by points; lethal -> Dead tag with
  respawn delay) + `update_respawns` (timers tick in place, respawns applied in
  ascending entity-id order, reused scratch -> no per-tick alloc; restores health
  + moves to spawn). Tests: armor-vs-health split, capping, death tagging,
  timed respawn + move, and cross-world determinism. 595/595 green.
  PIVOT (per Charter §4): deferred the "client/server split in the player path"
  (editor-gated, can't verify headlessly, and better done once gameplay exists to
  replicate) in favour of building the gameplay the netcode will carry. Next:
  **G — weapons (hitscan + projectile) applying damage via the lag-comp rewind.**
- (iter 5) **Hitscan + projectile weapons in `engine/gameplay`.** Weapon +
  Projectile components; `fire_hitscan` (ready-check spends ammo + sets cooldown,
  ray-vs-sphere nearest Health target excluding shooter, ties by lower id, applies
  weapon damage), `fire_projectile` (spawns a moving Projectile entity carrying
  the damage), `tick_weapons` (cooldowns), `tick_projectiles` (integrate, proximity
  damage to the first non-owner Health entity, despawn on hit/ttl; gather-then-
  mutate in id order for determinism). Tests: damage-on-hit + fire-rate gating +
  ammo, off-axis miss still costs a round, empty weapon can't fire, projectile
  travel/impact/despawn + ttl despawn, cross-world determinism. 601/601 green.
  Caveats: spread (random cone) deferred — needs a deterministic seeded RNG so it
  stays lockstep-safe; wiring the net session's lag-comp HitEvents to apply_damage
  (net-id <-> Entity map) lands with the client/server split. Next: **G —
  rounds/scoring/spawn points + pickups (health/ammo/weapon).**
- (iter 6) **Scoring + pickups in `engine/gameplay`.** Score{frags,deaths} +
  `damage_credited(attacker,victim,amount)` (frag to killer unless self-kill,
  death to victim); weapons (hitscan + projectile) now credit the shooter/owner.
  Pickup{kind,amount,radius,respawn_delay,cooldown} + `spawn_pickup` +
  `tick_pickups`: an active pickup grants to the first overlapping player (Health
  entity, lowest id) — Health heals capped at max_hp, Ammo/Weapon refill the
  Weapon — then goes inactive for respawn_delay; deterministic (players in id
  order, additive grants). Tests: frag/death crediting, self-kill, health heal +
  cap + respawn, ammo refill, out-of-range no-take, cross-world determinism.
  607/607 green. Deferred: round timer/phases + spawn-point selection (match
  orchestration) — lighter follow-up. Next: **A — DOTS combat AI: flow-field /
  navmesh pathing on the spatial broadphase (research first).**
- (iter 7) **Deterministic grid flow-field pathing — new `engine/ai` lane.**
  Chose flow-field over per-agent navmesh A* for the DOTS mass-agent fit: O(1)
  per-agent lookup, one shared field for thousands of agents (the standard RTS
  cost-field-from-goal + gradient technique). `FlowField`: resize a 2D XZ grid,
  block_aabb obstacles (half-open cells so a wall flush to a grid line leaves the
  next cell free), build = Dijkstra cost field from the goal (integer 10/14 edge
  costs, min-heap keyed by (cost, index) for deterministic ties, diagonals don't
  cut blocked corners), then each free cell's flow = toward its lowest-cost
  neighbour (ties -> lowest index). sample(world) returns the unit XZ steering
  dir. Tests: open-ground gradient toward goal, full wall -> unreachable, wall
  with a gap routes around, build determinism (cost+dir grids bit-identical).
  611/611 green. Determinism flags on the lane; perf_guardrails auto-covers
  engine/ai. Next: **A — combat bots: agents steer along the flow field + shoot
  through the gameplay systems; scale test + golden determinism.**
