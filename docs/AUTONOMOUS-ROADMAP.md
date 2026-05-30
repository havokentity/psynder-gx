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
- [~] Client/server split in the player path (server ticks authoritative; client
      predicts) — headless server smoke. **Net→gameplay bridge DONE (iter 11):
      `engine/match` MatchSession drives the real gameplay ECS off the netcode.
      Headless dedicated-server binary `PsyServerGX` DONE (iter 12): runs
      MatchSession server-authoritative + deterministic, builds in the dedicated
      config. Match orchestration wired in DONE (iter 15): PsyServerGX now runs a
      REAL match — warmup→active→intermission, frag limit, deterministic spawn
      selection on death. Remaining follow-up: bind the real UDP transport
      (net::Server + UdpSocket) + wire into the in-window 02_crate player.**

### G — Gameplay slice (FPS)
- [x] Weapons: hitscan + projectile, fire rate, ammo; damage application; tests.
      **DONE (iter 5).** (Spread + wiring net HitEvents to apply_damage on the
      server lag-comp path = follow-ups with the client/server split.)
- [x] Health/armor/damage/death/respawn components + systems (deterministic).
      **DONE (iter 4) — new `engine/gameplay` lane.**
- [x] Rounds/scoring/spawn points; pickups (health/ammo/weapon). **Scoring +
      pickups DONE (iter 6); rounds + spawn-point selection DONE (iter 14):
      engine/gameplay/MatchRules — Warmup→Active→Intermission phase machine,
      frag-limit + time-limit win conditions, frag-leader (id-tie), and
      farthest-from-enemy deterministic spawn selection.**
- [~] FPS controller polish (air control, crouch/jump tuning) on the Jolt capsule.
      **Deterministic Quake3 movement kernel DONE (iter 13):
      engine/physics/core/PlayerMovement (pm_friction/pm_accelerate/pm_move) —
      ground accel+friction, air-strafe, jump, crouch; unit-tested. Wiring it
      into CharacterSpine/the 02_crate pawn (replacing instant-velocity) = the
      in-window follow-up.**

### A — DOTS agents / AI
- [x] Navmesh or flow-field pathing (research first). **DONE (iter 7) —
      deterministic grid flow-field in the new `engine/ai` lane.**
- [x] Combat bots that path + shoot through the gameplay systems; scale +
      determinism test. **DONE (iter 8).** Scale follow-ups DONE (iter 16):
      UniformGrid broadphase enemy-search (O(bots·neighbours), identical results)
      + team-aware friendly-fire filter in fire_hitscan (shoots through
      teammates). Team relocated to GameplayComponents.h.

### M — Maps / world
- [~] Quake3-class arena: BSP/brush geometry + PVS cull wired into render +
      collision; a `samples/arena` demo. **Headless combat-loop essence DONE
      (iter 9): two flow-field bot teams fight deterministically in an arena.
      Visual BSP geometry + render + a samples/arena binary = follow-up (needs
      in-window verification).**
- [~] BF-light larger outdoor map (heightfield/streaming groundwork).
      **Deterministic CPU terrain query DONE (iter 17):
      engine/world/outdoor/HeightfieldQuery (terrain_height/normal/raycast/
      clamp_to_ground + offline generate_hills) — the gameplay/physics side of
      the heightfield, strict-FP. ECS integration DONE (iter 18):
      TerrainAgents (GroundClamp component + apply_terrain_clamp) snaps DOTS
      agents/movers onto the surface — agents walk hills deterministically.
      Streaming + GPU CDLOD draw of a real BF-light map = the in-window
      follow-up.**

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
- [~] Behavior IR → DOTS compiler growth (ADR-018); more nodes; hot reload;
      drive a gameplay behavior from a graph. **Executable Behavior IR DONE
      (iter 19): engine/script/behavior/BehaviorIR — a register-machine IR +
      deterministic interpreter over SoA streams (the middle layer between the
      VisualGraphCompiler front-end and the BehaviorSpine SIMD execution).
      Front-end lowering DONE (iter 20): VisualGraphCompiler::lower_graph_to_ir
      compiles a node graph (input/const/output + add/sub/mul/div/neg/min/max/
      cmp*/select) straight to a BehaviorProgram — a graph drives a real ECS
      behavior, no Lua. Live-ECS binding DONE (iter 21): a strided in-place
      execute runs a lowered program over for_each_chunk component columns (a
      graph heals real gameplay::Health entities). The MathLogicKernel SIMD
      back-end for the IR = follow-up.**

### D — Demos (test harness while editor matures)
- [~] `samples/arena` (Quake3), crowd-combat, destruction sandbox, vehicle test,
      net-play headless demo — one per milestone as needed. **Headless combat
      demos DONE: arena_combat (indoor, iter 9), outdoor_skirmish (terrain, iter
      22), PsyServerGX (networked match, iter 12/15). Visual sample binaries =
      in-window follow-ups.**

### P — Perf & determinism
- [x] Cross-platform golden parity: strict-FP the sim path (scene/math used by
      agents+physics) so Linux/Windows match macOS; flip determinism.yml off
      continue-on-error where it holds. **DONE: math + scene strict-FP (iter 10);
      Linux determinism REQUIRED (iter 10); Windows determinism unblocked (Vulkan,
      iter 10) + promoted to REQUIRED (iter 12) after two green pushes. The full
      mac+linux+windows determinism matrix is now required — no continue-on-error.**
- [ ] Alloc audits (no per-frame heap in hot paths); SIMD/job scaling; profiling.

## Journal

> Append one entry per iteration: what shipped, decisions/assumptions, sources,
> follow-ups, anything needing eventual human/in-window/PC-Vulkan check.

- (iter 41) **SOLO INTEGRATION #3 — tactical-AI skirmish (items A + D). Composes
  the unwired AI primitives into one deterministic bot loop.** New
  `tests/unit/tactical_ai_skirmish.cpp` extends the iter-30 skirmish pattern with
  the session's AI helpers: ai::TargetSelect (pick the best enemy by composite
  score) + ai::ThreatMemory (remember a last-seen enemy and SEARCH it after losing
  LoS) + ai::CoverScore over ai::CoverPoints (when hurt, pick the safest wall-
  adjacent cover) + ai::line_of_sight (acquisition gate) + ai::NavAgent (A* nav) +
  gameplay::TacticalBot (Patrol/Engage/Retreat FSM) + WeaponLoadout/fire_hitscan/
  Damage. Two 4-bot teams fight across a walled chokepoint: each bot builds enemy
  candidates, select_target picks one, records a sighting in ThreatMemory if
  visible, the FSM decides engage/retreat, a Retreating bot navigates to a
  CoverScore-chosen cover cell, and a Patrolling bot with no visible enemy
  searches its freshest remembered position. Asserts the loop fights, target
  acquisition + threat memory fire (any_sighting), the Retreat+CoverScore branch
  fires (any_retreat), AND the whole thing is bit-reproducible. Pure composition
  (no new engine code), compiled + passed on the FIRST run. Local: mac-debug ctest
  **1265/1265** (+2), mac-release determinism+tactical 108/108 (golden pin intact),
  perf_guardrails OK, PsyServerGX --ticks=128 + crate smokes exit 0. Three
  integrations now (netcode send iter 39, combat shot iter 40, tactical AI this) —
  the iter-26..38 building blocks are increasingly composed into working systems.
  Follow-ups: lift the skirmish into a visual samples/ binary (in-window);
  ThreatMemory-driven flanking; squad coordination via SquadCommand. (Continuing
  the user's "more integration + more additive waves" directive.)

- (iter 40) **SOLO INTEGRATION #2 — combat shot resolution (item G). Composes the
  per-shooter combat modifiers into one shot pipeline.** New
  `engine/gameplay/CombatResolve.{h,cpp}` gives the fire path a single
  `begin_shot(world, shooter, base_spread_tan) -> ShotResult` +
  `resolve_damage(...)` that wires together four systems built this session:
    * Magazine (Reload, iter 35) — the ammo gate: a shooter with a magazine must
      have a round and not be mid-reload; firing spends one. No magazine => fires
      freely (infinite-ammo bots).
    * Suppression (iter 38) — multiplies the spread cone (a suppressed shooter is
      less accurate).
    * Powerups (iter 33) — QuadDamage scales outgoing damage 4x.
    * RangedDamage / Ballistics (iters 25/26) — distance falloff + hitbox shaping.
  All components are OPTIONAL (a shooter missing one gets that modifier's neutral
  default), so it is backward-compatible — existing fire paths with none of them
  are unchanged. The test (`gameplay_combat_resolve.cpp`) proves each gate +
  modifier and their COMPOSITION: a full mag fires + spends a round, an empty or
  reloading mag is blocked (no round spent), suppression widens the cone, Quad
  scales damage 4x, all three compose on one shooter (round spent + 2x spread +
  4x headshot-falloff damage), and the resolution is deterministic. Compiled clean
  AND passed on the FIRST run — no fixes. Local: mac-debug ctest **1263/1263**
  (+7), mac-release determinism+combat 107/107 (golden pin intact),
  perf_guardrails OK, PsyServerGX --ticks=128 + crate smokes exit 0. Two
  integrations now consolidate the building blocks: the netcode SEND path
  (iter 39) and the combat SHOT path (this). Follow-up: adopt begin_shot/
  resolve_damage inside fire_hitscan (the determinism-critical wiring) so bots/
  players actually run through the gate; BattleSuit incoming-damage reduction on
  the victim side. Next integration candidates: tactical-bot AI depth (ThreatMemory
  + TargetSelect + CoverScore + Blackboard into the skirmish), or the netcode
  delta-codec variant of the send scheduler.

- (iter 39) **SOLO INTEGRATION — the bandwidth-managed snapshot SEND pipeline
  (item N / DoD bandwidth budget). User pivoted the loop from additive waves to
  INTEGRATION (composing the ~40 building blocks into real systems).** New
  `engine/net/SnapshotScheduler.{h,cpp}` answers the real question "what does the
  server send each peer this tick?" by COMPOSING four net primitives built this
  session:
    * PriorityAccumulator (iter 34) — accumulate each entity's send-priority;
    * BandwidthBudget (iter 38) — a token-bucket byte budget = send-rate * MTU;
    * pack_quantized / SnapshotQuantized (iters 24/25) — quantize + pack the
      chosen entities to a compact cross-platform-bitwise payload;
    * fragment_message / FragmentReassembler (iter 37) — split to the link MTU.
  Each tick: accumulate priorities -> take the highest-priority entities the
  budget affords (one record = quantized_wire_bytes()) -> select() resets the
  winners so the losers climb (no starvation) -> pack -> fragment. A peer
  reassembles + unpacks to recover exactly the scheduled records. The test
  (`net_snapshot_scheduler.cpp`) proves the END-TO-END loop: top-priority within
  budget, a peer round-trips the fragments back to the sent states (positions
  within the quant step), out-of-order fragments still reassemble, a tiny budget
  throttles to 1 record/tick, NO entity is starved over 60 ticks, an empty budget
  sends nothing, and the whole pipeline is bit-deterministic. ONE bug fixed during
  integration (a real composition lesson): the scheduler computed affordable
  records via budget.available_bytes()/20, but a float refill of "60 bytes" lands
  at 59.9997 -> floor 59 -> 2 records not 3 (and a 1-record budget gave 0 -> an
  OOB crash ASan caught); fixed by computing affordable with a sub-byte epsilon
  tolerance (same class of fix as iter-38's budget). Local: mac-debug ctest
  **1256/1256** (+7), mac-release determinism+scheduler 106/106 (golden pin
  intact), perf_guardrails OK, PsyServerGX --ticks=128 + crate smokes exit 0.
  This is the first of the consolidation passes — the netcode send path now
  composes priority + budget + quantize + fragment into one deterministic
  pipeline. Follow-ups: wire SnapshotScheduler into ReplicationSession's per-peer
  send (the determinism-critical step); a delta variant (DeltaBitCodec vs a
  per-peer baseline) instead of full quantized pack; AoI feeding the priority
  bases. Next integration candidates: tactical-bot AI depth (ThreatMemory +
  TargetSelect + CoverScore + SquadCommand + Blackboard into the skirmish), or
  combat depth (Reload + Suppression + Powerups + falloff into fire_hitscan).

- (iter 38) **6-LANE WAVE #7 (user-driven max-throughput) — six more additive
  features across six DISTINCT lanes, all NEW files, ZERO edits to existing/hot/
  golden code.** Shipped:
  1. **gameplay/Suppression** — incoming-fire suppression meter (12 B): add/tick-
     decay + spread_multiplier (suppressed = less accurate) + is_suppressed.
  2. **ai/Blackboard** — fixed-slot integer-keyed AI scratch (float/int/vec3/bool
     with a type-tag rule: a wrong-type get returns false). For behavior trees/FSMs.
  3. **net/BandwidthBudget** — token-bucket send byte budget: configure(rate,
     burst) + refill/can_spend/try_spend. The cap companion to PriorityAccumulator.
  4. **world/outdoor/TerrainCollision** — sphere-vs-terrain resolve: penetration +
     push-out along the normal + reflect_velocity bounce. Lockstep-safe.
  5. **camera/Spring** — critically-damped SmoothDamp spring (no trig, polynomial
     reciprocal) for smooth camera follows / value chases.
  6. **audio/Crossfade** — equal-power crossfade (cos/sin, constant power) + a
     timed Fader for music/ambience transitions.
  Integration: tree had EXACTLY 18 new files, ZERO modified tracked files;
  registered count healthy at 1249. ONE fix (4 failing assertions, one root
  cause): BandwidthBudget compared the f64 fill with EXACT `>=`, but a float
  refill of "100 bytes" (e.g. 10000*0.01) lands a hair under/over 100, so a
  nominal-N bucket couldn't spend N and "spend everything" left sub-byte residue
  that fails `Approx(0.0)` (no margin). FIXED IN THE IMPL (the correct fix, not the
  test): can_spend tolerates a sub-byte epsilon and try_spend snaps a drained-
  within-epsilon bucket to exactly 0 — robust to the inevitable float refill error.
  Local: mac-debug ctest **1249/1249** (+56), mac-release determinism 96/96
  (golden pin intact), perf_guardrails OK, PsyServerGX --ticks=128 + crate smokes
  exit 0. Total now: **75 features** (SEVEN 6-lane waves this session, 42 in the
  user burst). NOTE: cron still DELETED. Follow-ups: Suppression into the spread
  path; Blackboard backing TacticalBot; BandwidthBudget + PriorityAccumulator
  capping the replication send; TerrainCollision for grenades/rolling props;
  Spring/Crossfade in-window.

- (iter 37) **6-LANE WAVE #6 (user-driven max-throughput) — six more additive
  features across six DISTINCT lanes, all NEW files, ZERO edits to existing/hot/
  golden code.** Shipped:
  1. **gameplay/Killfeed** — fixed-capacity recent-kills ring (push/tick-age/
     newest-first read/evict-oldest) for the HUD.
  2. **ai/CoverScore** — score cover positions vs threats: cover_score (reward
     nearest-threat distance with a floor, penalize anchor distance) + best_cover
     (lowest-index tie). Pure structs (ai core+math).
  3. **net/Fragment** — packet fragmentation + reassembly: fragment_message (LE
     6-byte header: id/index/count) + FragmentReassembler (out-of-order tolerant,
     dup/malformed rejection, new-id reset). The rUDP large-message primitive.
  4. **match/RoundTimer** — time-driven round clock: Warmup->Active->(Overtime)->
     Ended with remainder carry across phase boundaries. Complements MatchRules'
     score-based win conditions.
  5. **camera/Transition** — death/kill-cam pose blend: smoothstep ease + shortest-
     arc yaw (no long-way spin across +/-180). Strict-FP.
  6. **audio/Compressor** — master-bus peak compressor/limiter: target gain
     (ratio above threshold, hard ceiling) + attack/release envelope.
  Integration: tree had EXACTLY 18 new files, ZERO modified tracked files;
  registered count healthy at 1193 (bracket rule held). ONE fix: the CoverScore
  best_cover test expected the on-anchor candidate to win, but with the safety-
  dominant kDefaultCoverWeights the safest (over-extended) candidate genuinely
  scored higher; gave that scenario anchor-dominant weights so the balanced pick
  wins (impl correct, test intent restored). Local: mac-debug ctest **1193/1193**
  (+62), mac-release determinism 92/92 (golden pin intact), perf_guardrails OK,
  PsyServerGX --ticks=128 + crate smokes exit 0. Total now: **69 features** (SIX
  6-lane waves this session, 36 features in the user burst). NOTE: cron still
  DELETED. Follow-ups: Killfeed fed from damage_credited kills; CoverScore +
  CoverPoints picking a bot's retreat spot; Fragment in the rUDP send path;
  RoundTimer into MatchSession; Transition/Compressor in-window.

- (iter 36) **6-LANE WAVE #5 (user-driven max-throughput) — six more additive
  features across six DISTINCT lanes, all NEW files, ZERO edits to existing/hot/
  golden code.** Every agent prompt now carries the "no brackets in TEST_CASE
  names" rule (iter-35 lesson) — clean run, registration healthy, no fixes needed.
  Shipped:
  1. **gameplay/Crouch** — crouch FSM: Crouch component (20 B) + crouch_update
     (ease height, can't stand under a ceiling via blocked_above) + crouch_fraction
     + crouch_speed_mult.
  2. **ai/SquadCommand** — squad orders (Advance to a rally / Hold / Regroup) +
     squad_member_goal (composes Formation Wedge slots) + squad_all_arrived.
  3. **net/CongestionControl** — Gaffer good/bad-mode send-rate adapter: demote on
     bad RTT, promote only after a sustained good period AND a min bad dwell
     (hysteresis). Feeds off RttEstimator.
  4. **match/Loadout** — class kits (Assault/Scout/Heavy) with primary/secondary
     WeaponClass + start health + speed mult; apply_loadout equips the pawn.
  5. **camera/WeaponSway** — viewmodel sway: the gun lags opposite the look
     velocity then recenters. Strict-FP, no trig.
  6. **audio/ReverbZone** — reverb wet/decay from room volume (saturating map) +
     reverb_lerp for crossfading zones + dry gain.
  Integration: tree had EXACTLY 18 new files, ZERO modified tracked files;
  compiled clean AND passed the full ctest on the FIRST run; registered test count
  healthy at 1131 (the bracket rule held — no discovery corruption). Local:
  mac-debug ctest **1131/1131** (+49), mac-release determinism 87/87 (golden pin
  intact), perf_guardrails OK, PsyServerGX --ticks=128 + crate smokes exit 0.
  Total now: **63 features** (FIVE 6-lane waves this session, ~30 features in the
  user-driven burst). NOTE: cron still DELETED. Follow-ups: Crouch height into the
  Jolt capsule; SquadCommand driving a bot squad; CongestionControl gating the
  replication send rate; Loadout into MatchSession spawns; WeaponSway/ReverbZone
  in-window.

- (iter 35) **6-LANE WAVE #4 (user-driven max-throughput) — six more additive
  features across six DISTINCT lanes, all NEW files, ZERO edits to existing/hot/
  golden code.** Shipped:
  1. **gameplay/Stamina** — sprint resource: Stamina component (24 B) +
     tick_stamina (drain while sprinting, regen after a delay, sprint resets the
     regen cooldown) + can_sprint/stamina_fraction/tick_staminas.
  2. **ai/ThreatMemory** — decaying last-seen-enemy memory: record/tick(forget)/
     find/freshest (smallest age, lowest-id tie)/active_count, fixed-capacity with
     oldest-eviction. Bots search a stale position after losing LoS.
  3. **net/RttEstimator** — Jacobson/Karels EWMA (RFC 6298): add_sample, srtt,
     rttvar (jitter), rto = clamp(srtt+4*rttvar), interp_delay_s. Drops bad
     samples; feeds the adaptive interpolation delay + retransmit timeout.
  4. **world/outdoor/TerrainMaterial** — surface classification by steepness +
     elevation (Grass/Rock/Snow/Sand, steepness-first) + material_friction. Drives
     footstep sounds + surface friction; lockstep-safe (updot gate, no acos).
  5. **camera/LookSmoothing** — exponential mouse-look low-pass: look_smooth
     (alpha = 1-smoothing; smoothing 0 = raw passthrough), NaN-guarded. Strict-FP.
  6. **audio/Footsteps** — distance-driven step cadence: footstep_advance (a step
     per stride travelled, multi-stride on a big move) + footstep_progress. The
     "faster movement = faster footsteps" timing.
  Integration: tree had EXACTLY 18 new files, ZERO modified tracked files. THREE
  fixes: (a) the terrain_material TEST called terrain_height/terrain_slope_updot
  for sanity asserts without including their headers (added 2 includes);
  (b) **IMPORTANT LESSON** — the audio_footsteps TEST_CASE NAME contained `[0,1)`,
  whose `[...]` catch_discover_tests parses as a Catch TAG, which CORRUPTED test
  registration and silently collapsed the WHOLE suite from 1029 to 148 registered
  tests (and produced spurious failures). Renamed the case ("0 to 1"); registration
  jumped back to 1082. **Rule: NEVER put square brackets in a TEST_CASE NAME — only
  in the tag string.** (c) the ai_threat_memory freshest test mis-expected id 3
  while enemy 2 was also still at age 0 (no tick between records) -> added a tick
  so 5/3 are the sole freshest; the impl's lowest-id tie-break was correct. Local:
  mac-debug ctest **1082/1082** (+53), mac-release determinism 81/81 (golden pin
  intact), perf_guardrails OK, PsyServerGX --ticks=128 + crate smokes exit 0.
  Total now: **57 features** (four 6-lane waves this session). NOTE: cron still
  DELETED. Follow-ups: Stamina gating sprint speed; ThreatMemory feeding
  TacticalBot search; RttEstimator sizing SnapshotInterp's delay; TerrainMaterial
  -> Footsteps surface sound selection; LookSmoothing into the 02_crate input path.

- (iter 34) **6-LANE WAVE #3 (user-driven max-throughput) — six more additive
  features across six DISTINCT lanes, all NEW files, ZERO edits to existing/hot/
  golden code.** Clean run, no agent stalls. Shipped:
  1. **gameplay/Reload** — weapon magazine/reload FSM: a `Magazine` component
     (in_mag/reserve/mag_size/reload timers, 20 B) + can_fire/consume_round/
     start_reload/reloading/tick_reload (reserve-limited top-up)/tick_reloads.
     Additive, sits alongside any Weapon.
  2. **ai/TargetSelect** — deterministic best-enemy selection: target_score
     (visible_bonus - distance_weight*d + low_health_bonus*(1-hp_frac)) +
     select_target (different-team, in-range, max score, lowest-id tie). Pure
     structs (no ECS dep — ai links only core+math).
  3. **net/PriorityAccumulator** — the Glenn-Fiedler snapshot scheduler: each
     entity's priority accumulates by its base each tick; select(K) sends the
     top-K and resets them to 0 so a starved entity climbs until it wins
     (anti-starvation, tested). Reused scratch, no per-call heap.
  4. **match/Objective** — a capture-point (Domination/KOTH): ControlPoint +
     tick_control_point (sole occupant drives progress, flips ownership at full;
     contested/empty/owner decays), sole_occupant/is_contested. Pure state machine.
  5. **camera/Lean** — peek/lean around cover: LeanState + lean_input (L/R/both/
     neither) + lean_update (clamp-eased) + lean_sample (lateral slide + roll,
     right => +lateral/+roll). Strict-FP, no trig.
  6. **audio/Ducking** — sidechain ducking: DuckEnvelope + duck_update (attack
     down to floor when triggered, release back to unity), duck_gain/duck_target.
     The "lower the music when the radio calls" envelope.
  Integration: tree had EXACTLY 18 new files, ZERO modified tracked files;
  compiled clean AND passed the full ctest on the FIRST run — NO fixes needed.
  Local: mac-debug ctest **1029/1029** (+57, crossed 1000 tests), mac-release
  determinism 78/78 (golden flock digest #1013 intact), perf_guardrails OK,
  PsyServerGX --ticks=128 + crate smokes exit 0. Total now: **51 features** (three
  6-lane waves this session). NOTE: cron still DELETED. Follow-ups: Reload wired
  to fire_hitscan (gate fire on can_fire, auto-reload on empty); TargetSelect
  feeding CombatBot/TacticalBot; PriorityAccumulator scheduling the replication
  send under a byte budget; Objective driving a KOTH PsyServerGX; Lean/Ducking
  in-window.

- (iter 33) **6-LANE WAVE #2 (resumed after a token-limit stall) — six more
  additive features across six DISTINCT lanes, all NEW files, ZERO edits to
  existing/hot/golden code.** The six wave-2 agents hit the session token limit
  mid-flight: two finished (gameplay/Powerup, world/outdoor/TerrainPathCost),
  three left only their header (ai/Formation, net/SequenceBuffer, camera/FovKick),
  one wrote nothing (audio/DistanceDelay). On resume I COMPLETED the partial lanes
  myself from the agent-authored headers + specs (wrote Formation.cpp+test,
  FovKick.cpp+test, the SequenceBuffer test, and all of DistanceDelay) rather than
  re-spawning, to avoid another stall. Shipped:
  1. **gameplay/Powerup** — Quake powerups: PowerupKind {QuadDamage, Haste,
     Regeneration, BattleSuit}, a `Powerups` timer component (16 B), grant/has/
     tick, damage_multiplier (4x quad) / incoming_damage_multiplier (0.5 suit),
     tick_regeneration (overheal cap). Deterministic, ascending id.
  2. **ai/Formation** — squad formation slots: formation_slot/formation_slots for
     Line (abreast), Column (single file), Wedge (V), with the Camera.h
     right-handed basis (right = cross(forward,+Y)), zero-forward fallback. The
     cohesion complement to per-agent nav.
  3. **net/SequenceBuffer** — the Gaffer-on-Games reliable-UDP primitive:
     seq_greater/seq_diff (16-bit wraparound-safe) + a header-only
     SequenceBuffer<T> ring (slot = seq % cap, stale-slot invalidation on a newer
     insert, too-old rejection). The backbone a future ack/jitter/fragment layer
     shares.
  4. **world/outdoor/TerrainPathCost** — slope-weighted outdoor traversal cost:
     terrain_move_cost (1.0 flat, rising to a cap, kImpassableCost past min_updot),
     terrain_passable, terrain_edge_cost (distance * avg endpoint cost). Lockstep-
     safe (updot gate, no acos).
  5. **camera/FovKick** — dynamic FOV: fov_target (ADS > sprint > base priority),
     fov_update (framerate-independent clamp-blended ease, no overshoot, guarded
     dt). Strict-FP, no trig. The FOV analog of Recoil/Shake.
  6. **audio/DistanceDelay** — speed-of-sound propagation: propagation_delay_s
     (d/343), delay_samples (rounded mixer offset), air_absorption_gain
     (1/(1+a*d)), distant_arrival bundle. The "flash before the boom" model.
  Integration: tree had 17 new files, ZERO modified tracked files. TWO trivial
  fixes: the camera test needed a `<limits>` include (mine), and the agent's
  TerrainPathCost edge-cost test used the map CORNER (0,0) where terrain_normal's
  central difference samples off-map and reads as a cliff -> moved it to interior
  points (impl behaviour is correct boundary handling). Local: mac-debug ctest
  **972/972** (+45), mac-release determinism 72/72 (golden flock digest #956
  intact), perf_guardrails OK, PsyServerGX --ticks=128 + crate smokes exit 0.
  Total now: **45 features** (two 6-lane waves back-to-back). NOTE: the cron driver
  is still DELETED (deleted iter 32); recreate via the SETUP PROMPT in
  docs/RESUME-AUTONOMOUS.md to resume autonomous cadence. Follow-ups: Powerup
  damage_multiplier into fire_hitscan; Formation driving squad steering;
  SequenceBuffer backing a real ack codec; TerrainPathCost into an outdoor A*;
  FovKick/DistanceDelay in-window.

- (iter 32) **6-LANE WAVE (user-driven max-throughput) — six additive features
  across six DISTINCT lanes, all NEW files, ZERO edits to existing/hot/golden
  code.** The user stopped the cron (a fire had stalled) and asked to "do as much
  as possible now", so I drove a wider fan-out (6 concurrent agents) directly.
  Shipped:
  1. **gameplay/WeaponInventory** — POD multi-weapon inventory (owned bitmask +
     per-class reserve ammo + active slot, 28 B) over WeaponLoadout: give_weapon
     (no refill on re-pickup), switch_to, cycle_next/prev (skips unowned, wraps),
     equip_active (projects the active archetype's WeaponSpec + restores reserve
     ammo into the entity's Weapon). FPS weapon switching.
  2. **ai/Influence** — a deterministic XZ influence/threat map: add_source
     (linear Euclidean falloff cone, accumulates, negative = friendly control),
     value(), down_gradient (flee) / up_gradient (seek) with lowest-index ties.
     The tactical-positioning complement to FlowField.
  3. **net/DeltaBitCodec** — the BIT-level quantized snapshot delta (BitPacker +
     SnapshotQuantized): per-field [6b width][zigzag(curr-prev)] so unchanged
     fields cost 6 bits and small moves a handful — strictly smaller than the
     20-byte/record SnapshotPackDelta (asserted). The real bandwidth win.
  4. **match/TeamScore** — TDM scoring: team_frags/deaths (sum Score by Team via
     for_each_chunk<Score,Team>), leading_team (lowest-index tie), and
     team_frag_limit_reached. The team analog of MatchRules' FFA frag_leader.
  5. **camera/ViewBob** — cosmetic walk view-bob: distance-driven phase, figure-8
     (vertical = sin(2*phase), lateral/roll = sin(phase)), guarded. Freezes when
     stopped. Same-platform strict-FP (sin OK — cosmetic, documented).
  6. **audio/VoiceCull** — deterministic voice limiting: voice_score (priority *
     inverse-distance attenuation), select_voices (top-K by score, inaudible
     dropped even with spare budget, lowest-id ties, descending order). The
     finite-voice-budget selector.
  Integration: tree had EXACTLY 18 new files, ZERO modified tracked files;
  compiled clean on the first build. ONE trivial fix: the Influence test
  mis-expected an overlap sum (10 vs the correct 12 — the second radius-5 source
  DOES reach the first's centre at dist 4); the implementation was right, fixed
  the test. Local: mac-debug ctest **927/927** (+51), mac-release determinism
  67/67 (golden flock digest #911 intact), perf_guardrails OK, PsyServerGX
  --ticks=128 + crate smokes exit 0. Total now: **39 features.** NOTE: the cron
  driver (job 62384f44) was DELETED this turn — the loop no longer auto-fires;
  recreate it (SETUP PROMPT in docs/RESUME-AUTONOMOUS.md) to resume autonomous
  cadence. Follow-ups: bots using Influence to pick safe positions; the
  DeltaBitCodec in the replication wire path; WeaponInventory + cycle wired to
  input; TeamScore driving a TDM PsyServerGX; ViewBob/VoiceCull in-window.

- (iter 31) **4-AGENT BATCH #8 — explosions + netcode compression + audio/feel,
  broadening into the audio lane: four additive features across four distinct
  lanes (G + N + audio + camera), all NEW files, ZERO edits to existing/hot/golden
  code.** Thematically coherent (a grenade boom: Splash damage + Shake trauma +
  the SpatialCue for the sound + BitPacker compressing the netcode). Agents
  forbidden from build/git/shared-or-other-lane files; I serially integrated.
  Shipped:
  1. **gameplay/Grenade** (item G) — a thrown, gravity-arcing, FUSE-timed grenade
     that detonates via iter-29's apply_splash_damage: `Grenade` component
     (velocity + fuse + SplashParams + owner/team, 48 B), `throw_grenade`
     (spawns with normalized velocity), `tick_grenades` (gravity integrate +
     fuse countdown + detonate-and-despawn, ascending-id gather-then-mutate,
     reused scratch). Composes Splash into a usable weapon. The agent pre-empted
     the MSVC `near`/`far` `<windows.h>` macro trap by renaming test locals.
  2. **net/BitPacker** (item N/P) — the sub-byte bit packer SnapshotPack/Metrics
     kept deferring to: `BitWriter`/`BitReader` (LSB-first within each byte, fixed
     by shifts/masks so it is host-endianness-independent), `write_bits`/
     `read_bits` for any width [0,64], overrun-latching reader, `bits_needed`
     (ceil log2), and zigzag encode/decode (incl. INT64_MIN) for small signed
     deltas. Packs eight 5-bit values into 5 bytes — the real compression win.
  3. **audio/SpatialCue** (audio) — the world-geometry-to-scalars front-end that
     feeds the existing PositionalMix: `source_distance_m`, `distance_attenuation`
     (inverse-distance rolloff, clamped), `listener_azimuth` (atan2 of the
     source projected onto the listener's forward/right basis, rear hemisphere
     folded to the same-side hard-pan edge — never flips L/R), `doppler_pitch`
     (c/(c-closing), clamped [0.5,2]), + a `spatialize` bundle. Non-overlapping
     with PositionalMix (which it documents feeding). Cosmetic, same-platform.
  4. **camera/Shake** (feel) — trauma-based screen shake, the impact analog of
     iter-29's Recoil: `ShakeState`(trauma+time+seed) + `shake_add_trauma`/
     `shake_tick`(decay)/`shake_sample`. Magnitude = trauma^2 (smooth ease-out);
     each of the 6 axes gets independent splitmix64 VALUE NOISE (lattice hash +
     smoothstep lerp over time*frequency, NO trig) so it is strict-FP bit-
     identical. An explosion adds trauma; the camera punch eases out.
  Integration: tree had EXACTLY 12 new files, ZERO modified tracked files. ONE
  trivial fix (same kNoTeam-needs-Weapons.h test include as iter-29's Splash; a
  one-line add). Local: mac-debug ctest **876/876** (+39), mac-release determinism
  62/62 (golden flock digest #860 intact), perf_guardrails OK, PsyServerGX
  --ticks=128 + crate smokes exit 0. Eight 4-agent batches + one solo integration
  now: **33 features.** Follow-ups: TacticalBot/CombatBot lobbing grenades
  (throw_grenade) at clustered enemies; BitPacker driving a tighter
  SnapshotPackDelta wire codec; SpatialCue+PositionalMix wired into the audio
  mixer pull path for real positional gunfire; an explosion adding camera Shake
  trauma in 02_crate (in-window). Next: the determinism-critical SnapshotStream->
  ReplicationSession threading / IR SIMD back-end (SOLO), or a BitPacker-backed
  delta codec.

- (iter 30) **SOLO — tactical-bot integration: prove the iter-26..29 building
  blocks COMPOSE (items A + D + M).** After seven additive batches (28 features,
  many unwired), the disciplined move was to consolidate via integration rather
  than pile on more helpers. Did this SOLO (keystone composition, not fan-out).
  Shipped:
  1. **engine/gameplay/TacticalBot.{h,cpp}** — a reusable, deterministic tactical
     FSM: `enum TacticalState {Patrol, Engage, Retreat}`, a POD `TacticalBot`
     component (state + retreat_health_frac), and a PURE `decide_tactical_state(hp,
     max_hp, enemy_visible, retreat_frac)` — fight-or-flight (a hurt bot retreats
     even with a visible enemy; else engage on sight; else patrol). No World/RNG/
     alloc, so it is trivially testable + lockstep-safe. + a 6-case unit test
     (priority, visibility gate, threshold edges, clamp + degenerate-health guard,
     ECS storability, determinism).
  2. **tests/unit/tactical_skirmish.cpp** — the headless integration harness (the
     proven arena_combat pattern, extended): two teams of 5 bots fight across a
     24x24 walled arena with ONE chokepoint, composing — in ONE deterministic loop
     — ai::NavAgent (A* + string-pull routes through the gap), ai::line_of_sight
     (target acquisition gate — no shooting through the wall), ai::CoverPoints
     (a Retreating bot falls back to the nearest wall-adjacent cover cell),
     gameplay::WeaponLoadout (equips a MachineGun + its RangedDamage falloff into
     fire_hitscan), the TacticalBot FSM, and the existing Weapons/Damage/respawn
     systems. Read-phase decide+fire (ascending id) then write-phase navigate+move
     (the §1b order-independent pattern), pure-kinematic movement (decoupled from
     the physics/agents hot path). Asserts the composed loop actually fights
     (frags+deaths>0), the Retreat branch + CoverPoints really fire (any_retreat),
     AND the whole thing is bit-reproducible across two runs — the real proof that
     every composed subsystem is lockstep-safe.
  Integration: 4 new files (2 lane + 2 test), ZERO edits to existing/hot/golden
  code; one trivial self-fix (a `const World&` helper -> `World&` since
  World::get is non-const). Everything passed on the FIRST test run. Local:
  mac-debug ctest **837/837** (+8), mac-release determinism+tactical 67/67 (golden
  flock digest #821 intact; the skirmish is bit-reproducible), perf_guardrails OK,
  PsyServerGX --ticks=128 + crate smokes exit 0. This validates the iter-26..29
  navigation/cover/arsenal/falloff/FSM stack works TOGETHER, not just in isolation
  — the DoD wants things composed. Follow-ups: in-window visual of the skirmish;
  give bots strafing/lead while Engaging; a SnapshotStream-replicated skirmish
  (server runs it, client interpolates via SnapshotInterp). Next: back to a
  4-agent additive batch, OR the determinism-critical SnapshotStream->
  ReplicationSession threading / IR SIMD back-end (SOLO).

- (iter 29) **4-AGENT BATCH #7 — combat feel + client smoothing, broadening into
  the camera lane: four additive features across four distinct lanes (G + A + N +
  camera), all NEW files, ZERO edits to existing/hot/golden code.** Deliberately
  spread into a lane I had not touched (camera) to avoid saturating gameplay/ai/
  net. Agents forbidden from build/git/shared-or-other-lane files; I serially
  integrated. Shipped:
  1. **gameplay/Splash** (item G) — radial blast damage for rockets/grenades:
     `apply_splash_damage(world, center, SplashParams{inner,outer,max}, attacker,
     friendly_team, scratch)` damages every Health entity within the outer radius,
     scaled by 3D distance via Ballistics::damage_at_distance (full inside inner,
     0 at outer), gather-then-apply in ascending entity-id order, credited to the
     attacker. Self-damage ON (classic rocket-jump); friendly-team filter mirrors
     fire_hitscan (kNoTeam = FFA). Lockstep-safe (one sqrt/candidate).
  2. **ai/CoverPoints** (item A) — tactical cover detection on the nav grid:
     `is_cover_cell` (free cell with >=1 blocked cardinal neighbour; open border is
     NOT cover), `find_cover_cells` (ascending-index list), `cover_direction`
     (4-bit wall-side mask: +X/-X/+Z/-Z), `nearest_cover_cell` (closest by squared
     grid distance, lowest-index tie). Pure integer, deterministic — the positions
     a bot moves to for cover behind walls.
  3. **net/SnapshotInterp** (item N) — client-side render-smoothing buffer: a
     fixed-capacity time-sorted ring of snapshots; `sample(render_time, out)`
     brackets the time, lerps each shared entity's position + SHORTEST-ARC yaw
     (lerp_yaw_deg via wrap180, no trig) between the two snapshots, clamps to the
     endpoints with NO extrapolation, single-side ids pass through. The standard
     interpolation-delay client smoothing (cosmetic; pure/deterministic).
  4. **camera/Recoil** (FPS feel) — deterministic weapon recoil/spray for the
     view: `RecoilState`/`RecoilPattern` + `recoil_fire` (pitch climbs to a cap,
     horizontal weave from a local splitmix64 hash of (seed, shot_index) -> signed
     unit in [-1,1], NO trig), `recoil_recover` (decays both offsets toward 0,
     resets shot_index when fully recovered), `recoil_reset`. Strict-FP camera
     lane => same-platform bit-identical (replay-safe). Pairs with WeaponLoadout.
  Integration: tree had EXACTLY 12 new files, ZERO modified tracked files. ONE
  trivial fix: the Splash test referenced `kNoTeam` (defined in Weapons.h) without
  including it — a one-line `#include "gameplay/Weapons.h"` in the test, the only
  issue across all four lanes. Local: mac-debug ctest **829/829** (+33), mac-release
  determinism 58/58 (golden flock digest #813 intact), perf_guardrails OK,
  PsyServerGX --ticks=128 (real 2-frag match) + crate smokes exit 0. Seven 4-agent
  batches now: **28 features across 7 CI cycles.** Follow-ups: fire_projectile/a
  rocket weapon detonating via apply_splash_damage on impact; bots picking
  CoverPoints under fire (Patrol -> nearest_cover when threatened); the client
  driving SnapshotInterp off received snapshots in 02_crate (in-window); the FPS
  pawn applying camera Recoil per shot (in-window). Next: a SOLO CombatBot
  tactical integration (Patrol + NavAgent + WeaponLoadout + TerrainVisibility +
  CoverPoints), or the determinism-critical SnapshotStream->ReplicationSession /
  IR SIMD back-end (SOLO).

- (iter 28) **4-AGENT BATCH #6 — bot-combat-depth: four additive features across
  four distinct lanes (G + A + M + P), all NEW files, ZERO edits to existing/hot/
  golden code.** A coherent batch deepening the combat loop while staying fully
  disjoint + headless. Agents forbidden from build/git/shared-or-other-lane files;
  I serially integrated. Shipped:
  1. **gameplay/WeaponLoadout** (item G) — the Quake3 arsenal as a deterministic
     constexpr data table: `WeaponClass {Railgun, RocketLauncher, Shotgun,
     MachineGun, Plasma}`, a `WeaponSpec` (damage / fire_interval / max_ammo /
     hit_radius / hitscan / projectile_speed / FalloffProfile), `weapon_spec()`
     (metric values, MachineGun fallback for out-of-range — no UB), and
     `equip_weapon(Weapon&, class)` filling the shared Weapon POD + arming it
     ready. Reuses iter-26's FalloffProfile so each archetype carries its falloff.
     Numbers: Rail 80/1.5s/10 hitscan rifle-falloff; Rocket 100/0.8s/10 proj 30m/s;
     Shotgun 60/1.0s/10 hitscan shotgun-falloff; MG 7/0.1s/100 hitscan pistol-
     falloff; Plasma 20/0.1s/50 proj 40m/s.
  2. **ai/Patrol** (item A) — a multi-point patrol sequencer over iter-27's
     NavAgent: a `PatrolRoute` (ordered grid points + cursor + loop flag),
     `start_patrol`, and `update_patrol` (squared-XZ arrival test against the
     current point's cell centre -> advance cursor, wrap when looping or hold the
     last point, set_goal only when the target actually changes to avoid replan
     thrash -> delegate to NavAgent::update for the steer). Bots now walk routes.
  3. **world/outdoor/TerrainVisibility** (item M / tactical AI) —
     `terrain_line_of_sight(a, b, clearance)` + `terrain_los_clearance(a, b)`:
     march the segment in ~one-texel steps and require each interior sample clear
     the bilinear terrain_height; the min interior clearance backs both (LoS ==
     clearance >= c by construction). Pure algebra over the heightfield, lockstep-
     safe — the "can a bot at A see/shoot B over the hills" query.
  4. **net/SnapshotMetrics** (item P) — bandwidth accounting tooling: a POD
     `BandwidthMeter` (total/frames/peak) + `meter_record`/`reset`,
     `mean_bytes_per_tick`/`bytes_per_second`/`bits_per_second`,
     `compression_ratio`/`savings_fraction` (delta vs full), and
     `full_snapshot_bytes` = packed_size. Guarded f64, measurement-only (documented
     not-authoritative). Lets us measure the SnapshotStream/Delta saving.
  Integration: tree had EXACTLY 12 new files, ZERO modified tracked files;
  **compiled clean AND passed the full ctest on the first try — no fixes needed**
  (cleanest batch alongside iter-26). Local: mac-debug ctest **796/796** (+29),
  mac-release determinism 54/54 (golden flock digest #780 intact), perf_guardrails
  OK, PsyServerGX --ticks=128 (real 2-frag match) + crate smokes exit 0. Six
  4-agent batches now: **24 features across 6 CI cycles.** Follow-ups: equip bots/
  players from WeaponLoadout + pass the spec's falloff into fire_hitscan; drive a
  CombatBot off Patrol+NavAgent (patrol then engage on LoS); use TerrainVisibility
  to gate bot target acquisition; surface SnapshotMetrics in PsyServerGX/a bench.
  Next: a CombatBot integration tying Patrol+NavAgent+WeaponLoadout+TerrainVisibility
  into a headless tactical-bot demo (gameplay, likely SOLO), or the determinism-
  critical SnapshotStream->ReplicationSession threading / IR SIMD back-end (SOLO).

- (iter 27) **4-AGENT BATCH #5 — the WIRING batch: land iter-26's building blocks
  into call sites, two backward-compatible wirings + two new streaming layers,
  four DISTINCT lanes.** Since three of the four iter-26 follow-ups target the
  gameplay lane (one lane = one parallel agent), I spread the work across four
  distinct lane dirs and mixed 2 backward-compatible opt-in wirings (gameplay,
  match) with 2 additive new-file lanes (net, ai). Agents forbidden from building/
  git/touching shared or other-lane files; I serially integrated. Shipped:
  1. **RangedDamage -> fire_hitscan** (gameplay, backward-compatible): appended a
     trailing `const FalloffProfile* falloff = nullptr` to fire_hitscan. nullptr
     (the default — every existing caller incl. the combat bots) credits the flat
     `wp->damage` EXACTLY as before, bypassing all distance arithmetic, so the
     determinism path is bit-identical; a non-null profile scales by
     `resolve_ranged_damage(wp->damage, best_t, Hitbox::Body, *falloff)` where
     best_t is the hit distance along the normalized ray. Closes "Ballistics ->
     fire_hitscan".
  2. **SpawnValidation -> MatchSession** (match, backward-compatible): appended an
     optional `f32 min_walkable_updot = 0.0f` to configure_terrain. When the gate
     is active (has_terrain && min_walkable_updot > 0) the kill/respawn selection
     filters the spawn ring to the WALKABLE subset (filter_walkable_spawns) before
     the farthest-from-enemies pick, so a frag never drops a victim onto a cliff;
     empty-walkable-set falls back to the full ring (never spawn-less). Default 0
     => byte-identical to before. Closes "terrain_walkable -> spawn validation".
  3. **net/SnapshotStream** (additive): the stateful baseline-tracking layer over
     iter-26's stateless SnapshotPackDelta. SnapshotStreamSender::encode packs a
     delta vs its baseline then advances the baseline to the DEQUANTIZED round-trip
     of curr (dequantize(quantize(curr))) — so sender and receiver track the
     IDENTICAL evolving baseline and an entity that moved < one quant step (omitted
     from the delta) can never desync the two sides. Receiver applies + adopts the
     reconstructed set; validate-before-mutate on truncation. The real
     send-a-delta-each-tick wire path. Closes "SnapshotPack -> wire path".
  4. **ai/NavAgent** (additive): the per-agent "navigate to a world point"
     controller a CombatBot embeds, over iter-26's PathFollow: a NavAgent owns a
     goal + a PathFollower; set_goal arms + forces a replan; update() lazily
     replans (world_to_cell start -> plan_world_path) and returns the unit XZ
     steer (zero when arrived / no path). Adds the world_to_cell inverse (clamps
     off-grid positions to the nearest edge cell). Closes "PathSimplify/A* ->
     a bot".
  Integration: tree had EXACTLY the 4 wiring-lane edits (Weapons.{h,cpp},
  MatchSession.{h,cpp} — distinct dirs, no shared-file edits) + 8 new files;
  compiled clean on the first build. Two NEW-lane tests over-asserted and I fixed
  them (the only integration work): the NavAgent test required `follower.current
  == 0` after an update, but the agent starts AT waypoint[0] (the start cell) so
  follow_steer correctly consumes it -> relaxed to "actively walking a fresh,
  not-yet-exhausted route"; the SnapshotStream sub-step-jitter test assumed a
  2-3mm nudge never crosses a 1cm quant step, but positions near a cell boundary
  do -> snap t0 onto the lattice centre (x=q*res) first, giving the full ±5mm
  margin the test claims. Both fixes are test-only; the implementations were
  correct (no impl change, no golden movement). Local: mac-debug ctest **767/767**
  (+21), mac-release determinism 53/53 (golden flock digest #751 intact),
  perf_guardrails OK, PsyServerGX --ticks=128 (real 2-frag match) + crate smokes
  clean. Five 4-agent batches now: **20 features across 5 CI cycles.** The two
  backward-compatible wirings (opt-in trailing param, default bit-identical) prove
  the parallel cadence works for wiring, not just pure-additive files. Follow-ups:
  drive a CombatBot off NavAgent (path to a point + shoot); thread SnapshotStream
  into ReplicationSession's per-peer send (a determinism-critical SOLO edit);
  PsyServerGX outdoor match using configure_terrain(h, min_updot); a per-shot
  falloff profile on the Weapon component so bots/servers pass it into fire_hitscan.
  Next: the CombatBot+NavAgent wiring (gameplay, SOLO or batched), or the
  determinism-critical IR SIMD back-end / UDP transport done SOLO.

- (iter 26) **4-AGENT BATCH #4 — four additive helpers across four distinct
  lanes (the safest shape again: all NEW files, ZERO edits to existing/hot/golden
  code, zero CMake edits — every lane GLOBs + is already linked into the unit
  test target).** Spawned 4 general-purpose agents concurrently, each forbidden
  from building / running git / touching shared or other-lane files; I serially
  integrated (one configure + build + full ctest + determinism + smoke + commit +
  push + CI watch). Shipped:
  1. **ai/PathFollow** — the per-agent "A* -> string-pull -> steer" execution
     layer over GridAStar + simplify_path: `GridLayout` + `cell_to_world` (cell
     index -> world XZ centre), `plan_world_path` (find_path -> simplify_path ->
     world waypoints), and a `PathFollower` with `follow_steer` (unit XZ dir to
     the current waypoint, squared-distance arrival advance, zero vector at the
     end). One guarded sqrt; no trig/RNG. Closes the iter-25 "PathSimplify into an
     A* bot" follow-up.
  2. **net/SnapshotPackDelta** — the quantized snapshot DELTA codec SnapshotPack's
     header defers to: `pack_quantized_delta(prev, curr, res, out)` emits a
     removed-id list + only the changed/added 20-byte records (quantized-field
     compare, ascending-id order so the bytes are input-order-independent),
     `apply_quantized_delta` reconstructs curr from baseline + delta (validates
     fully before mutating `out`, false on truncation), `delta_size`. Mirrors
     SnapshotPack.cpp's exact LE byte helpers (no struct memcpy). The bandwidth
     bit-packer over iter-24/25's quantized snapshot form.
  3. **world/outdoor/SpawnValidation** — walkable-spawn filtering on the
     heightfield: `filter_walkable_spawns` (ascending indices passing the
     lockstep-safe `terrain_walkable` cos-threshold gate), `clamp_walkable_spawns`
     (survivors snapped to surface + foot offset via `clamp_to_ground`),
     `any_walkable_spawn`/`first_walkable_index`. Closes "terrain_walkable into
     spawn validation". No acos (uses the updot gate), pure algebra.
  4. **gameplay/RangedDamage** — composable per-weapon falloff + hit-region
     classifier over Ballistics: a POD `FalloffProfile` + canonical metric
     profiles (kNoFalloff identity, kRifleFalloff {35,90,0.60}, kPistolFalloff
     {12,35,0.45}, kShotgunFalloff {6,18,0.15}), `resolve_ranged_damage` (one call
     -> Ballistics::ranged_damage), and `classify_hitbox(hit_y, foot_y, height)`
     (top>=0.85 -> Head, bottom<=0.30 -> Limb, else Body, height<=0 guard) — the
     building block fire_hitscan needs to know WHICH hitbox a ray struck. Additive
     (no component/Weapons edits).
  Integration: verified the tree had exactly 12 new untracked files and ZERO
  modified tracked files before building; **compiled clean on the FIRST build with
  zero fixes needed** (the cleanest batch yet — past batches each needed one
  trivial Approx/RNG fix). Local: mac-debug ctest **746/746** (+31), mac-release
  determinism+new 53/53 (golden flock digest #730 intact), perf_guardrails OK
  (the new RNG-free algebra has no forbidden constructs), PsyServerGX --ticks=128
  (real 2-frag match) + crate smokes exit 0. Four clean 4-agent batches now: **16
  features across 4 CI cycles.** Follow-ups (now genuine wiring of these building
  blocks): PathFollow into a CombatBot that paths to a point; SnapshotPackDelta
  into the ReplicationSession wire path; SpawnValidation into MatchRules/
  MatchSession spawn selection; RangedDamage (falloff + classify_hitbox) into
  fire_hitscan. Next: a wiring batch landing those into call sites (backward-
  compatible opt-in), or the determinism-critical IR SIMD back-end / UDP transport
  done SOLO.

- (iter 25) **4-AGENT BATCH #3 — four additive helpers, four distinct lanes.**
  The safest batch shape (all NEW files, zero edits to existing/hot/golden code):
  1. **net/SnapshotPack** — portable little-endian pack/unpack of a quantized
     entity snapshot (explicit LE byte writes, not struct memcpy) + truncation
     guard; the wire-serialization layer over iter-24's SnapshotQuantized.
  2. **ai/PathSimplify** — deterministic integer Bresenham `line_of_sight` +
     greedy string-pull `simplify_path` turning a GridAStar cell path into corner
     waypoints (no-corner-cut rule preserved).
  3. **gameplay/Ballistics** — `damage_at_distance` (full→min linear falloff,
     degenerate-input guards) + `Hitbox` multipliers (Head 2×, Body 1×, Limb
     0.7×) + `ranged_damage`; pure algebra, lockstep-safe.
  4. **world/outdoor/TerrainSlope** — `terrain_slope_updot` (normal.y = cos slope)
     + `terrain_walkable` (cos-threshold gate, the lockstep-safe form) +
     `terrain_slope_radians` (acos, documented query-only).
  Integration: 12 new files, ZERO modified tracked files. One agent test had a
  bare `Approx(` instead of `Catch::Approx(` (net_snapshot_pack.cpp:59) — a
  one-line fix, the only issue across all four. Local: mac-debug ctest **715/715**
  (+26), mac-release determinism+new 66/66 (golden pin intact), perf_guardrails
  OK (Ballistics/SnapshotPack have no forbidden constructs), server+crate smokes
  exit 0. Three clean 4-agent batches now: **12 features across 3 CI cycles**,
  each needing at most a single trivial integration fix — the parallel cadence is
  proven reliable for additive disjoint-lane work. Follow-ups: wire Ballistics
  into fire_hitscan damage, SnapshotPack into the replication wire path,
  PathSimplify into an A* bot, terrain_walkable into spawn validation. Next: a
  wiring batch, or the IR SIMD back-end / UDP transport.

- (iter 24) **4-AGENT BATCH #2 — wire iter-23 primitives into call sites + a new
  AI pather.** Four agents, four disjoint lanes, one CI cycle (the new cadence):
  1. **Spread → fire_hitscan** (gameplay/Weapons): two optional params
     `spread_tan=0, spread_seed=0` appended after friendly_team — when >0 the unit
     aim is perturbed by `spread_direction` before the ray test; default 0 keeps
     every existing caller bit-identical.
  2. **SnapshotQuantized** (net, additive): `quantize_state`/`dequantize_state`
     (pos via net::quantize, yaw in milli-degrees) + `quantized_wire_bytes` — a
     bandwidth-efficient cross-platform-bitwise snapshot form.
  3. **Terrain-aware respawns** (match): opt-in `configure_terrain(HeightmapDesc)`
     — on a kill the chosen spawn point is clamped onto the surface
     (clamp_to_ground); match lane now LINKS psynder_world_outdoor. Backward-
     compatible (no terrain configured => raw spawn Y as before).
  4. **GridAStar** (ai, additive): deterministic point-to-point grid A* (integer
     10/14 costs, octile heuristic, explicit (f,index) min-heap tie-break, no
     corner-cutting) — the per-agent complement to the shared-goal FlowField.
  Integration: verified the tree had exactly the 4 lanes' edits + 8 new files
  (no shared-file edits), built clean on the first try. ONE agent test was
  RNG-geometry-flaky ("a wide cone eventually hits a far off-axis target" — 256
  seeds isn't enough vs a 0.5 m hitbox 5.4 m away); I replaced it with a robust
  deterministic check (a wide cone misses an on-axis target on *some* seeds AND
  hits on some — proving spread affects the ray) — the only fix needed across all
  four features. Local: mac-debug ctest **689/689** (+14), mac-release
  determinism+new 62/62 (golden pin intact; modified Weapons/MatchSession stayed
  deterministic), perf_guardrails OK, server+crate smokes exit 0. Follow-ups:
  wire spread_seed into the bots'/server's fire calls; use SnapshotQuantized in
  the wire codec; GridAStar into a bot that paths to a point; PsyServerGX outdoor
  match using configure_terrain. Next batch: those wirings, or the IR SIMD
  back-end + UDP transport.

- (iter 23) **4-AGENT PARALLEL BATCH — four disjoint additive features in one CI
  cycle.** At the user's request, switched the loop to fan-out: spawned 4 agents
  concurrently, each authoring ONE additive feature in a DISTINCT lane (new files
  only — existing lanes auto-GLOB, so ZERO shared-file edits, which is what
  eliminates the conflict that broke past parallel runs per Charter §5). Agents
  were forbidden from building / running git / touching shared or other-lane
  files; I serially integrated (one configure + build + full ctest + determinism
  + smoke + commit + push + CI watch — amortized across all four). Verified the
  working tree had exactly 11 new untracked files and ZERO modified tracked files
  before building. Shipped:
  1. **engine/script/behavior/BehaviorSystem.h** — type-safe `BehaviorSystem<Comp>`
     wrapper: binds IR stream slots to `f32 Comp::*` member fields + hoisted
     zero-alloc scratch, runs a graph program over `for_each_chunk<Comp>` (the
     reusable form of iter-21's manual demo; tested over real gameplay::Health).
  2. **engine/world/outdoor/TerrainSpawn.{h,cpp}** — `select_farthest_spawn`
     (farthest-from-enemy XZ pick, id-tie) + `pick_terrain_spawn` (clamps the
     choice onto the heightfield) — terrain-aware match spawns.
  3. **engine/net/Quantize.{h,cpp}** — deterministic fixed-point position
     quantize/dequantize (floor(x/res+0.5) in double; round-trip ≤ res/2) for
     bandwidth-efficient cross-platform-bitwise snapshots.
  4. **engine/gameplay/Spread.{h,cpp}** — the deferred (iter-5) lockstep-safe
     weapon spread: a splitmix64 `Rng` + `spread_seed(tick,entity,shot)` +
     `spread_direction` that perturbs within a cone using ONLY cross-product
     basis math + one sqrt (NO runtime trig, since libm sin/cos aren't
     cross-platform-identical) — provably `dot(base,result) >= cos(half_angle)`.
  All four compiled on the FIRST integration build (clean agent code). Local:
  mac-debug ctest **675/675** (+22), mac-release determinism+new 58/58 (golden
  pin intact), perf_guardrails OK (new RNG/quantize have no forbidden
  constructs), server+crate smokes exit 0. Decision: agents author disjoint
  files only (no build/git) + I own the serial integration — captures the
  parallel-implementation speedup AND the §5 isolation safety, without the
  cold-build/worktree-merge cost. This is the new default loop cadence when ≥3
  disjoint headless items exist. Follow-ups: wire Spread into fire_hitscan, the
  quantizer into the snapshot delta codec, TerrainSpawn into MatchSession's
  outdoor spawns, BehaviorSystem into a real gameplay system. Next: integrate
  these four into their call sites (a follow-up batch), or the IR SIMD back-end /
  UDP transport.

- (iter 22) **Outdoor skirmish — the Battlefield-light combat loop integrated
  headless (items D + M).** The outdoor analog of arena_combat (iter 9),
  composing everything the terrain track built: new `outdoor_skirmish.cpp` spawns
  two teams of 12 flow-field combat bots on a procedural heightfield
  (generate_hills), GroundClamp-tags them, and runs a full tick — tick_combat_bots
  (path + shoot through the broadphase + team friendly-fire) → update_agents (XZ
  steering) → apply_terrain_clamp (snap Y to the surface) → tick_weapons →
  update_respawns → tick_match (rounds + frag limit). Asserts the integrated
  outdoor loop actually fights (frags + deaths > 0), every bot stays EXACTLY on
  the terrain surface throughout, the match advances into a live round, and the
  whole thing is bit-reproducible across runs (per-bot hp/frags/x/z signature).
  This ties engine/world/outdoor (HeightfieldQuery + TerrainAgents, iters 17–18)
  + engine/ai (FlowField) + engine/physics/agents + engine/gameplay (CombatBot /
  Weapons / Damage / MatchRules, iters 5–16) into one deterministic Battlefield-
  light skirmish. Pure integration (no new engine code) — proves the systems
  compose outdoors. NOTE: this iteration was interrupted mid-gate by a
  permissions crash that left the smoke lock held; recovered by releasing both
  locks and re-running. Local: mac-debug ctest **653/653** (+1), mac-release
  determinism+outdoor 44/44 (golden pin intact), server+crate smokes exit 0.
  Next: a reusable BehaviorSystem wrapper, the IR SIMD back-end, the UDP
  transport binding, or terrain-aware match spawns.

- (iter 21) **A graph behavior runs as a system over the live ECS (item S /
  ADR-018 — closes "drive a gameplay behavior from a graph").** Connected iters
  19–20 to real component storage. Added a STRIDED in-place execute to BehaviorIR:
  `execute(prog, span<StreamColumn>, count)` where a StreamColumn is `{f32* base,
  usize stride}` — point it at a component field (stride = sizeof(Component)/
  sizeof(f32)) and the program reads/writes that column IN PLACE, no gather/
  scatter copy. Refactored the existing BehaviorChunk execute to delegate to it
  (stride-1 views), so there's one core interpreter. Test `behavior_system.cpp`:
  the iter-20 heal graph (now `hp<=25 ? min(hp+50, max_hp) : hp`) lowers to IR and
  runs over REAL `gameplay::Health` entities via `for_each_chunk<Health>` — hp at
  stride 2, max_hp the untouched odd lane — healing low entities (capped at
  max_hp: a 5/40 entity heals to 40, not 55), proven idempotent once healed, and
  bit-deterministic across worlds over 200 entities × 3 passes. This is the full
  ADR-018 loop end to end: PsyGraph JSON → Behavior IR → deterministic execution
  over the authoritative ECS, no Lua anywhere. Local: mac-debug ctest **652/652**
  (+3), mac-release determinism+behavior+graph 57/57 (golden pin intact),
  server+crate smokes exit 0. Decision: strided columns over ECS storage (zero
  copy, §1b-friendly) rather than gather/scatter; the per-chunk StreamColumn
  vector in the test would be hoisted to a reused scratch in a production system
  wrapper. Follow-ups: a reusable BehaviorSystem wrapper (program + stream→
  component binding, hoisted scratch), the MathLogicKernel SIMD back-end for the
  IR, effect ops (spawn/destroy). Next: the IR SIMD back-end, a BehaviorSystem
  wrapper, the UDP transport, or terrain-aware match spawns.

- (iter 20) **PsyGraph → Behavior IR lowering — a graph drives a real DOTS
  behavior (item S / ADR-018).** Connected iter-19's executable IR to the graph
  front-end. New `VisualGraphCompiler::lower_graph_to_ir(graph_json)` (reuses the
  lane's existing JSON parser + node model) compiles a node graph straight into a
  `behavior::BehaviorProgram`: each node → one IR register (inputs reference
  earlier nodes, document order), with ops `input` (LoadStream), `const`,
  `output` (StoreStream), `add/sub/mul/div`, `neg` (× −1), `min/max`,
  `cmple/cmplt/cmpge/cmpgt`, and `select`; the result carries a `streams[]`
  name→slot map so the caller binds ECS component columns to the program's
  stream slots. This is the user's "PsyGraph → native DOTS, away from Lua" path:
  the SAME graph that `compile_visual_graph` lowers to Lua TEXT now lowers to an
  executable deterministic IR instead. Tests (5): an arithmetic graph computes
  v*2+1 over a chunk, a threshold+select rule heals low health (the iter-19
  hand-built behavior, now authored as a graph and producing identical results),
  a multi-stream graph reads two columns + writes a third, malformed graphs fail
  with diagnostics (bad JSON / no nodes / unknown op / missing inputs), and
  lowering is deterministic. Local: mac-debug ctest **649/649** (+5), mac-release
  determinism+graph+behavior 51/51 (golden pin intact), server+crate smokes exit
  0. Decision: added the lowering INTO VisualGraphCompiler.cpp (same psynder_script
  lane as BehaviorIR) to reuse its JsonParser/node helpers — zero new parser, no
  cross-lane dep. Follow-ups: bind the program's streams to live ECS chunks (run
  a graph as an actual gameplay system over for_each_chunk), the MathLogicKernel
  SIMD back-end, effect ops (spawn/destroy) in the IR, hot reload. Next: the IR
  SIMD back-end, run-a-graph-over-the-ECS binding, the UDP transport, or
  terrain-aware match spawns.

- (iter 19) **Executable Behavior IR — the DOTS middle of PsyGraph (item S /
  ADR-018).** The two ends existed but were disconnected: VisualGraphCompiler
  (graph JSON → Lua/C++ TEXT) is the front-end, BehaviorSpine is a HAND-LOWERED
  proof of the SIMD execution model — nothing in between actually ran an authored
  graph as DOTS. New `engine/script/behavior/BehaviorIR.{h,cpp}`: a compact
  register-machine IR (Load/Store-stream, Const, Uniform, Add/Sub/Mul/Div, fused
  Madd, Min/Max, the four compares, Select) + a deterministic interpreter that
  runs it entity-major over SoA f32 component columns, with a single up-front
  register-scratch alloc (no per-entity heap) and a `BehaviorBuilder` DSL for
  authoring. Every op is pure algebra/compare (no transcendentals/RNG), so under
  the script lane's NEW strict-FP flag the result is bitwise identical across
  arm64/x86_64/MSVC — this is the data-driven shape a PsyGraph lowers to, decoupled
  from Lua (the user's "away from Lua → compile to native DOTS" direction). Tests
  (3): a projectile semi-implicit-Euler behavior matches a reference integration
  over 90 ticks, a threshold+select behavior flags & heals low health, and a
  64-entity/200-tick run is bit-deterministic (memcmp). Added
  psynder_determinism_fp to the script lane (behavior code is lockstep sim; Lua
  builds as a separate target, unaffected — no existing VM/spine test regressed).
  Local: mac-debug ctest **644/644** (+3), mac-release determinism+behavior 46/46
  (golden pin intact), server+crate smokes exit 0. Decision: a self-contained
  interpreter (plain strict-FP loops) rather than lowering into MathLogicKernel
  this pass — zero coupling risk; the SIMD back-end (IR → MathLogicKernelBuilder,
  which BehaviorSpine already shows by hand) is the natural follow-up. Follow-ups:
  graph-JSON → IR lowering (wire VisualGraphCompiler's node set to emit IR), the
  MathLogicKernel SIMD execution path for the IR, hot reload, drive a real ECS
  gameplay system from a graph. Next: UDP transport binding, terrain-aware match
  spawns + outdoor PsyServerGX skirmish, or the IR SIMD back-end.

- (iter 18) **Bind ECS movers to the heightfield — agents walk the hills (item
  M).** Made iter-17's terrain query usable by the DOTS ECS. New
  `engine/world/outdoor/TerrainAgents.{h,cpp}`: a `GroundClamp{foot_offset_m}`
  tag component + `apply_terrain_clamp(world, heightmap)` — a chunk pass over
  (TransformWS + GroundClamp) that resamples each entity's Y from the terrain at
  its XZ (leaving prev_mtw to whoever moved it, so motion interpolation stays
  correct). Order-independent + deterministic, strict-FP (world_outdoor lane), so
  it runs on the lockstep tick AFTER update_agents: the agent system steers in
  XZ, this snaps to the surface. Tests (3): GroundClamp snaps entities to known
  ramp heights (+ foot offset), a mover dragged across XZ tracks the surface, and
  — the integration proof — 8 DOTS agents seek a far corner over procedural hills
  for 120 ticks, each staying EXACTLY on the surface, advancing toward goal, and
  bit-reproducible across runs (a deterministic headless outdoor skirmish). This
  composes the agent steering (physics/agents) + the heightfield (world/outdoor)
  the same way arena_combat composed the indoor FPS loop. Local: mac-debug ctest
  **641/641** (+3), mac-release determinism+terrain 50/50 (golden pin intact),
  server+crate smokes exit 0. Decision: a separate clamp PASS rather than
  threading terrain into update_agents — keeps the agent system terrain-agnostic
  and the clamp reusable for players/props; the cost is two passes (acceptable,
  both are tight chunk loops). Follow-ups: slope-aware movement (steering cost
  uphill), terrain-aware match spawn placement (clamp spawn points), GPU draw of
  the hills (in-window). Next: **M visual draw** (in-window), the UDP transport
  binding, or terrain-aware spawns + an outdoor PsyServerGX skirmish (headless).

- (iter 17) **BF-light heightfield: deterministic CPU terrain query (item M).**
  The outdoor lane (engine/world/outdoor) had a GPU-render scaffold (CDLOD /
  raymarch) + an inline bilinear sampler, but NO public gameplay/physics ground
  query — the doc even anticipated "lane 13 collides against the same data". New
  `HeightfieldQuery.{h,cpp}` fills it, built on the shared `sample_bilinear` so
  gameplay collides against exactly the rendered data: `terrain_height`
  (bilinear), `terrain_normal` (central-difference of the bilinear field),
  `terrain_raycast` (stepped march + 24-iter bisection refine, with immediate-hit
  when starting under the surface), `clamp_to_ground` (snap Y + foot offset), and
  an OFFLINE `generate_hills` (sum-of-sines rolling terrain). Determinism: the
  runtime queries are pure algebraic + sqrt over the u16 data — cross-platform
  bitwise under the lane's new strict-FP flag (added psynder_determinism_fp to
  world_outdoor); generation uses sin() so it's explicitly OFFLINE CONTENT (the
  u16 map is serialized + shared, NOT regenerated per client, since libm sin
  isn't cross-platform identical — documented in the header). Tests (6): flat
  field (const height / up normal / clamp), +X ramp (linear interp + tilted
  normal), downward ray hits / upward ray misses / under-surface immediate hit,
  oblique ray lands on the surface, procedural hills reproducible + bounded +
  queryable, out-of-bounds returns 0. Local: mac-debug ctest **638/638** (+6),
  mac-release determinism+terrain 47/47 (golden pin intact; the world_outdoor
  strict-FP flag regressed no existing CDLOD/scatter test), server+crate smokes
  exit 0. Decision: reused the existing HeightmapDesc + inline sampler rather
  than a new heightfield type, so render + physics + gameplay share one source of
  truth. Follow-ups: agent ground-clamp + match spawn-on-terrain using these;
  GPU CDLOD draw + streaming of a real BF-light map (in-window). Next: **M —
  visual BSP/terrain draw** (in-window), the UDP transport binding, or wiring
  HeightfieldQuery into agents/match for an outdoor headless skirmish.

- (iter 16) **Combat-bot scale follow-ups: broadphase enemy search + team-aware
  friendly fire (item A / §1b perf).** The bot enemy search was O(bots·
  combatants) — fine for 64v64, a wall at thousands. Rewrote `tick_combat_bots`
  to build a `scene::UniformGrid` over point-AABB proxies of the live combatants
  (cell sized to the max fire range) and `query_sphere(pos, fire_range)` per bot,
  so the search is O(bots·neighbours). Critically it produces IDENTICAL results:
  the grid returns a superset, and the SAME in-range + nearest-by-lowest-id
  predicate runs on it — so the existing 64v64 determinism test and the arena
  combat test pass unchanged, proving equivalence. Also added a team-aware
  friendly-fire filter: `fire_hitscan` gained an optional `friendly_team`
  (default `kNoTeam` = FFA, so all existing callers are untouched); when set, the
  ray shoots THROUGH teammates to strike the enemy behind them. To support that
  cleanly, relocated the `Team` component from CombatBot.h to GameplayComponents.h
  (its natural home — players + bots + the weapon filter share it; the
  PSYNDER_COMPONENT id is the type-signature hash, so the move keeps the same id
  and all ECS storage/tests are unaffected). Tests: new friendly-fire test
  (FFA hits the nearer teammate; team-filtered shoots through to the enemy) +
  the unchanged bot/arena suite validates the broadphase. Local: mac-debug ctest
  **632/632** (+1), mac-release determinism+bots+arena 44/44 (golden pin intact),
  server+crate smokes exit 0. Decision: per-tick grid build (matches the existing
  per-tick combatant-gather allocation pattern); a scratch-reused zero-alloc pass
  is a separate perf item. Follow-ups: the same broadphase for the agent
  steering neighbour search; a true thousands-scale bot stress test. Next: **M —
  visual BSP arena** (in-window) or the UDP transport binding, or a BF-light
  heightfield (headless terrain groundwork).

- (iter 15) **Match orchestration wired into the server — a real deterministic
  match end-to-end.** Composed the systems built over iters 11–14: `MatchSession`
  gained `configure_match(MatchConfig, spawn_points)` and now (a) ticks
  `gameplay::MatchRules` each advance (Warmup→Active→Intermission phase machine,
  frag/time win conditions, winner), (b) gates damage on the Active phase
  (Warmup/Intermission are no-damage — net hits are drained but not applied), and
  (c) on each kill picks the victim's next respawn deterministically via
  `select_spawn` (farthest ring point from living enemies), writing it into the
  victim's Respawnable. `PsyServerGX` configures a warmup + frag limit + a
  4-point spawn ring and prints phase transitions + the winner: the headless run
  now shows `warmup → active(round 1) → MATCH OVER winner … → intermission →
  warmup → active(round 2)`, a genuine match flow. Tests: extended
  match_session.cpp — a configured session reaches Active, ends at the frag limit
  with the shooter as winner, the victim respawns onto a spawn-ring point (not
  its origin), and both the outcome AND the run are bit-deterministic. Local:
  mac-debug ctest **631/631** (+2), mac-release determinism+match 86/86 (golden
  pin intact), PsyServerGX real-match + crate smokes exit 0. Backward-compatible:
  no configure_match call => default no-limit rules, behaviour unchanged (the
  existing MatchSession tests stay green). Decisions: scores persist across
  matches (so round 2 can end instantly if a prior leader is already past the
  limit) — a per-match score reset is a caller policy / small follow-up; warmup
  gates damage but not movement. Follow-ups: reset scores on a new match; team
  modes; bind the UDP transport for a networked PsyServerGX. Next: **M — visual
  BSP arena** (in-window) or the UDP transport binding, or per-match score reset
  + a fuller PsyServerGX match (bots via CombatBot driving inputs).

- (iter 14) **Match orchestration — rounds + win conditions + spawn selection
  (item G, closing the gameplay slice).** The deferred iter-6 follow-up. New
  `engine/gameplay/MatchRules.{h,cpp}`: a deterministic match state machine
  `Warmup → Active → Intermission → Warmup` (`tick_match`) with Quake3-style win
  conditions — first to `frag_limit`, or highest score at `time_limit` — driving
  off the existing Score components; `frag_leader` (highest frags, ties → lowest
  entity id); and `select_spawn`, the classic anti-spawn-camp heuristic that
  picks the spawn point whose nearest LIVING enemy (Health.hp>0, excluding self)
  is farthest away, ties → lowest index. All allocation-free (per-candidate
  rescans, counts small), strict-FP, id-ordered — no RNG. Ties the Score /
  Health / TransformWS components into an actual match with a winner. Tests (6):
  warmup→active+round increment, frag-limit ends with the right winner, time-
  limit ends for the frag leader, leader id-tie break, spawn picks farthest-from-
  enemy (and ignores a DEAD enemy on the far point), and a scripted 400-tick
  match is bit-deterministic across runs. Local: mac-debug ctest **629/629**
  (+6), mac-release determinism+match 84/84 (golden pin intact), server+crate
  smokes exit 0, full matrix-bound CI. Decisions: team-agnostic spawn selection
  (FFA-deathmatch default; a team filter is a small add when team modes land);
  MatchState is a plain struct (one per session/server), not an ECS component,
  since it's singleton match state. Intermission cycles back to Warmup for
  continuous rounds; score reset between matches is the caller's call. Follow-up:
  drive MatchRules from PsyServerGX + MatchSession (the server runs real rounds);
  team modes. Next: **M — visual BSP arena** (in-window) or bind the UDP
  transport into PsyServerGX (networked dedicated server, headless), or wire
  pm_move/MatchRules into PsyServerGX for a fuller headless match demo.

- (iter 13) **Deterministic Quake3 movement kernel (item G — the tight arena
  feel).** The player capsule moved at instant velocity (arcade): the input
  layer set a desired horizontal velocity and Jolt clamped it — no acceleration,
  friction, or air control, so no Quake-arena feel and no bunnyhop. New
  `engine/physics/core/PlayerMovement.{h,cpp}`: a faithful METRIC port of id
  Software's `bg_pmove.c` — `pm_friction` (PM_Friction: control=max(speed,
  stop_speed), drop=control·friction·dt, crisp stop below an epsilon),
  `pm_accelerate` (PM_Accelerate: addspeed = wishspeed − dot(vel,wishdir),
  add ≤ accel·dt·wishspeed along wishdir — the one routine Quake3 uses for BOTH
  ground and air), and `pm_move` (friction when grounded, jump that sets vy +
  clears grounded + SKIPS friction so a hop preserves horizontal speed = the
  bunnyhop invariant, ground vs air accel constant, real gravity while
  airborne). Quake3 constants ported to metric (accel 10, air 1, friction 6,
  duck 0.25; speeds scaled to a 7 m/s run). PURE + strict-FP + no Jolt
  dependency, so it's headless-unit-testable and feeds the CharacterVirtual each
  tick (solver still owns collision; this owns feel). Tests (6): friction →
  crisp stop, ground accel ramps to the cap and NEVER exceeds it, **air-strafing
  exceeds the ground cap (air control proven)**, jump preserves horizontal speed
  + sets vy (= jump_speed − g·dt, gravity acts that tick — a real-physics detail
  the test caught), crouch caps at the duck scale, and the kernel is
  bit-deterministic over a varied 300-tick schedule. Local: mac-debug ctest
  **623/623** (+6), mac-release determinism+movement 43/43 (golden pin intact),
  server+crate smokes exit 0. Source: id-Software/Quake-III-Arena
  code/game/bg_pmove.c. Decision: kept real gravity (9.81, metric pillar) rather
  than Quake3's snappier 800 u/s² — tune later for feel if needed. Follow-up
  (in-window): wire pm_move into CharacterSpine / the 02_crate pawn to replace
  set_desired_horizontal_velocity, exposing crouch height + air control to the
  actual player. Next: **M — visual BSP arena geometry** (in-window) or bind the
  UDP transport into PsyServerGX (networked dedicated server, headless).

- (iter 12) **Headless dedicated-server binary `PsyServerGX` + item P fully
  closed.** Two coherent server/determinism deliverables:
  (1) **PsyServerGX** (`apps/PsyServerGX/`) — the concrete "runs
  server-authoritative" artifact the DoD calls for and the binary the dedicated
  build (`PSYNDER_GX_DEDICATED_SERVER=ON`) produces (previously the dedicated
  build compiled only libs, no executable). It runs `engine/match` MatchSession
  on a fixed 128 Hz tick with NO graphics/audio/editor, scripted client inputs
  standing in for connected players until the UDP transport is bound. Links only
  headless deterministic lanes (match→net+gameplay+scene+math+core), so it builds
  in BOTH the normal player config and the dedicated config — verified by
  configuring `-DPSYNDER_GX_DEDICATED_SERVER=ON` (PsyServerGX present,
  PsyArcadeGX correctly skipped, samples off) AND building+running it there
  (links from purely headless static libs). CMake: widened the `apps` guard to
  `BUILD_PLAYER OR DEDICATED_SERVER`; `apps/CMakeLists.txt` gates PsyArcadeGX to
  non-dedicated and always adds PsyServerGX. Smoke: `PsyServerGX --ticks=N`
  exits 0, server-auth frags/deaths/respawns play out, and run-to-run output is
  bit-identical (4-client/300-tick deterministic). Determinism FP on the target.
  (2) **Item P closed** — promoted the **Windows determinism** job off
  continue-on-error (green across iters 10–11 once the Vulkan SDK install
  unblocked configure). The full **mac + linux + windows determinism matrix is
  now REQUIRED**; no continue-on-error remains in determinism.yml. Local:
  mac-debug ctest 617/617 (incl perf_guardrails), PsyServerGX + crate smokes
  exit 0, dedicated-config build+run clean. Decisions: scripted inputs (not a
  live UDP loop) keep the server-tick smoke deterministic + CI-stable; binding
  net::Server + UdpSocket is the transport follow-up. Risk noted: with Windows
  determinism now required, a future MSVC-STL/`/fp:strict` divergence will (
  correctly) block the branch — fix-forward, don't re-mask. Next: **G — FPS
  controller polish (air control, crouch/jump on the Jolt capsule)** or bind the
  UDP transport into PsyServerGX (true networked dedicated server).

- (iter 11) **The net loop drives REAL gameplay — new `engine/match` lane (item
  N / DoD §8 bullet 1).** The netcode (ReplicationSession) and the gameplay ECS
  (Health/Score/Damage) were both proven but disjoint: the net layer ran
  lag-comp hit DETECTION on its own `EntityState`, never touching the gameplay
  Health. `MatchSession` is the keystone bridge — it owns a `ReplicationSession`
  (server-auth movement + client predict/reconcile + lag-comp ray test over the
  latency channel) AND a `scene::World` of one player Entity per client
  (Health/Score/Weapon/Respawnable/TransformWS), with the players_ vector as the
  net-id↔Entity map (id = client+1). Each advance(): tick the net session →
  mirror authoritative net positions into the ECS TransformWS → drain the newly
  registered HitEvents and apply each through the REAL `gameplay::damage_credited`
  (Health→Dead + Score, attacker's ECS Weapon damage) → `update_respawns`. A hit
  on an already-Dead (respawning) player is skipped, so no double-frag credit
  (deterministic on state, not timing). This closes the explicitly-deferred
  "wire net lag-comp HitEvents to apply_damage (net-id↔Entity)" follow-up from
  iters 5–6. Headless + deterministic + dedicated-server-safe (links only
  net+gameplay+scene+core+math, no GPU) — so it also advances the dedicated
  server. Determinism FP on the lane; perf_guardrails extended to lint
  engine/match AND to lock the math/scene/gameplay/ai strict-FP from iter 10
  against regression. Test `match_session.cpp`: 2 clients over a 3-tick latency
  channel, shooter fragging the victim through several death/respawn cycles —
  asserts server-auth damage+frags+deaths happened, the shooter (never targeted)
  stays full-health with no self-frags, AND the whole bridged loop is
  bit-deterministic across runs. Local: mac-debug full ctest **617/617** (+2),
  mac-release determinism+match subset 77/77 (golden pin intact), guardrails OK.
  Decision: net is authoritative for POSITION (respawn's TransformWS write is
  re-synced from net each tick) — fine for the headless bridge; an in-window
  client would drive its camera from `predicted()`. Follow-ups: a real
  dedicated-server binary (`samples/server` / tools) running MatchSession on the
  Server tick scheduler; wire MatchSession into the 02_crate player path
  (in-window verification); team-damage filter. Next: **G — FPS controller
  polish (air control, crouch/jump on the Jolt capsule)** or **M — visual BSP
  arena geometry** (needs in-window), so likely **G** or a dedicated-server demo
  binary off this lane.

- (iter 10) **Cross-platform FP parity — strict-FP the foundational sim lanes
  (item P).** The lockstep-critical hole: `engine/math` (Math.cpp matrix /
  transform kernels, Bounds, MathLogicKernel, VectorStack) and `engine/scene`
  (Bvh / Sap / SpatialIndex / UniformGrid broadphase + World.cpp transforms) —
  the base the whole sim (agents/physics/gameplay/ai) builds on — compiled their
  non-inline TUs with DEFAULT FP, i.e. fused-multiply-add contraction +
  reassociation allowed. That diverges between arm64 NEON and x86_64 SSE/AVX2
  FMA — exactly the cross-platform lockstep risk. Fix: `psynder_determinism_fp()`
  (cmake/HotLane.cmake: `-fno-fast-math -ffp-contract=off` / MSVC `/fp:strict`)
  on both lanes, matching physics/agents/ai/gameplay which already opt in.
  CRITICAL CHECK: rebuilt **mac-release** + ran the golden/determinism subset
  before pushing — the pinned macOS/arm64 golden flock digest (#599/#613) still
  MATCHES, so the change tightened Linux/Windows parity WITHOUT moving the
  authoritative value (the flock sim already ran strict at the agent-lane call
  sites; only the shared .cpp TUs change, and they weren't on the golden path).
  CI hardening: the determinism workflow's **Windows** job was silently failing
  at configure (`Could NOT find Vulkan`) — it never installed the SDK — masked
  by continue-on-error; added the silent LunarG SDK install (mirrors ci.yml) so
  it can actually build + run the MSVC-STL divergence check. **Linux** determinism
  (green across consecutive pushes) **promoted off continue-on-error → now
  REQUIRED**: a real arm64-vs-x86_64 divergence now fails the branch instead of
  hiding. Local: mac-release determinism 36/36 (golden pin intact), mac-debug
  full ctest 615/615 (incl perf_guardrails), headless crate smoke clean.
  Follow-ups: watch the Windows determinism job go green on this push, then
  promote it off continue-on-error too (next iter); then item P's remaining
  alloc-audit / SIMD-scaling half. Next: **G — FPS controller polish (air
  control, crouch/jump on the Jolt capsule)** or **N — client/server split in
  the player path** (the netcode loop's last headless-verifiable piece).

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
- (iter 8) **Combat bots: path via the flow field + shoot through the weapons.**
  Ties engine/ai (FlowField) + engine/gameplay (Weapons/Health/Score) + the
  agents component (scene::AgentTarget). Team + Bot components; `tick_combat_bots`
  (gameplay, now links psynder_ai): each bot steers along the sampled flow field
  (writes its AgentTarget a lookahead step along the flow dir toward the goal),
  and if the nearest live enemy of another team is within fire_range it
  fire_hitscans (gated on cooldown/ammo, credits the kill). Deterministic: bots
  processed in ascending id order, nearest-enemy ties by lower id. Tests: bot
  damages an in-range enemy + retargets along the field, holds fire out of range,
  and a 64v64 scale run is bit-identical across worlds (with engagement). 614/614
  green. Reordered CMake so engine/ai is added before engine/gameplay (which now
  links it). Follow-ups: SpatialIndex broadphase for the enemy search (true
  thousands-scale) + a team-aware friendly-fire filter in the hitscan ray. Next:
  **M — Quake3-class arena: a `samples/arena` demo (BSP/brush + PVS, flow-field
  bots fighting in it).**
- (iter 9) **Arena combat — the FPS stack integrated as a game (headless).** New
  test `arena_combat.cpp` wires engine/ai (FlowField) + engine/physics/agents
  (update_agents steering) + engine/gameplay (CombatBot/Weapons/Damage/Score/
  Respawn) into one loop: two teams of 16 flow-field bots converge on the arena
  centre, shoot each other through the weapon/health systems, die + respawn, and
  score. Asserts the integrated loop actually fights (frags > 0, deaths > 0) AND
  is bit-reproducible across two runs (per-bot hp/frags/position signature).
  615/615 green. PIVOT (Charter §4): delivered the milestone's deterministic
  combat-loop essence headlessly; the visual BSP/brush geometry + render + a
  samples/arena binary need in-window verification, so they're a follow-up.
  Next: **G — FPS controller polish (air control, crouch/jump on the Jolt
  capsule)** OR **P — cross-platform golden parity (strict-FP the sim path)**;
  pick the cross-platform determinism pass (P) — it hardens the lockstep pillar
  the whole stack now depends on, and is pure-determinism work well-suited to
  headless autonomy.
