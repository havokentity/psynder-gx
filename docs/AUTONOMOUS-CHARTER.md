# Psynder-GX — Autonomous Build Charter

> This is the master prompt for fully-autonomous, zero-input engine development.
> Each loop iteration: **read this file + `AUTONOMOUS-ROADMAP.md`, do one solid
> increment, prove it green, commit/push, watch CI, journal it, continue.**
> Never stop for human input. Press through every blocker. Never give up.

## 0. Prime directive

Build a **next-gen, high-performance DOTS ECS game engine** and a **compiling
player** that recreates **fast-paced multiplayer FPS games only** (Quake 3-class
arena + Battlefield-light scale). Ship it: deterministic, networked, with a
Unity-style WYSIWYG editor and PsyGraph visual scripting. Keep going until the
Definition of Done (§8) is met or the time budget is exhausted.

You operate with **zero input from the user**. If a design/editor question
arises, choose the sensible default, **record the assumption** in an ADR + the
roadmap journal, and **press on**. If genuinely stuck on feature X, pivot to
another roadmap item or **spin up a new demo/sample project** that exercises the
subsystem from a different angle — features built in a demo migrate into the
engine/editor later. Momentum over perfection; correctness over speed; never
idle waiting.

## 1. Product pillars (HARD constraints — never violate)

1. **Cross-platform bit-determinism.** Server-authoritative, 128-tick-capable
   lockstep (ADR-GX-005). Sim/physics/net TUs build `-fno-fast-math
   -ffp-contract=off`; iterate by stable entity id (ECS swap-remove makes
   storage order history-dependent). Golden determinism tests gate it.
2. **DOTS, always cache-coherent.** SoA, chunked iteration, `psynder::jobs`
   parallelism, **no per-frame heap allocation** in hot paths (reserve/reuse).
   No exceptions/RTTI/`std::shared_ptr` in renderer/scene/physics/net hot paths.
3. **`scene::World` is the single authoritative scene.** Every mover writes
   `TransformWS`. No parallel mutable world (Jolt/agents project from + write
   back to the ECS — ADR-019).
4. **Real metric units.** 1 world unit = 1 metre; real masses/forces; no
   demo-scaled shortcuts.
5. **GPU-accelerated identity.** Unified `psy::gpu` over Vulkan + native Metal.
   Lanes above gpu write API-neutral code (never raw `vk*`/`MTL*`).
6. **Determinism + tests are the bar.** Full `ctest` green and CI green before
   moving on. Watch CI from push #1 (libc++/MSVC-STL + platform-define gaps only
   surface on CI).

## 2. Scope & ordering (what to build, in what order)

**Raster first. Netcode + gameplay + editor in the middle. Ray tracing LAST**
(only reflections + shadows, nothing fancy — defer until the raster FPS loop is
complete and playable).

Target feel: **Quake 3 arena** (tight, fast, deterministic) first; then
**Battlefield-light** scale (bigger maps, vehicles optional/late, more players).
**FPS only** — no other genres.

Milestone themes (detailed, ordered backlog lives in `AUTONOMOUS-ROADMAP.md`):
- **N — Netcode core loop** (mostly done): lag-comp hitbox rewind into the loop;
  per-peer interest management; over-the-wire transport integration; a real
  client/server split in the player.
- **G — Gameplay slice**: FPS controller polish; weapons (hitscan + projectile),
  health/armor/damage, respawn, rounds/scoring, pickups.
- **M — Maps**: a Quake3-style BSP arena; a larger BF-light outdoor map.
- **A — DOTS agents/AI**: scale to thousands; navmesh/flow-field pathing; combat
  bots that play the FPS loop.
- **R — Rendering**: adopt `render::pipeline::render()` in the player; PBR-ish
  lighting; post (tonemap/bloom); **then, last,** minimal RT reflections+shadows
  behind a build flag.
- **E — Editor (WYSIWYG)**: scene authoring on the ECS (retire `ScenePrimitive`);
  transform gizmos; play-in-editor; entity inspector; asset browser; PsyGraph
  node editor; live IPC to the running player.
- **S — Scripting (PsyGraph)**: visual graph → DOTS compile (ADR-018); grow the
  node set; hot reload; drive gameplay from graphs.
- **D — Demos**: a demo/sample project per milestone (arena, BF-light,
  destruction sandbox, crowd-combat, vehicle test) — these are your test
  harness while the editor matures.
- **P — Perf & determinism**: cross-platform golden parity (strict-FP the sim
  path), profiling, alloc audits, SIMD/job scaling.

## 3. The per-iteration loop (do this every wake)

1. **Orient.** `cd` to the repo. `git status`; ensure on `nextgen/new-release`,
   clean. Read this charter + `AUTONOMOUS-ROADMAP.md` (state + journal).
2. **Pick** the highest-priority **unblocked** roadmap item. One coherent
   increment per iteration (a feature, a lane, a demo, a fix).
3. **Research** before non-trivial work: use `WebSearch`/`WebFetch` for
   algorithms + best practices (snapshot interpolation, lag comp, BSP/PVS,
   navmesh/flow-field, PBR, RT reflection without denoiser, ECS patterns, Jolt
   APIs, Vulkan/Metal specifics). Cite sources in the ADR/journal.
4. **Design.** For anything architectural, write/append an ADR under
   `docs/adr/`. Keep public headers stable; if a contract must change, note the
   cascade.
5. **Implement.** Solo for focused/core/determinism-critical work. For
   independent, disjoint-directory work, fan out **parallel Agent lanes** — but
   with **HARDENED ISOLATION** (see §5). Mandate DOTS + `jobs` + determinism in
   every lane-agent prompt.
6. **Prove it.** `cmake --preset mac-debug && cmake --build --preset mac-debug`;
   then full `ctest` GREEN (hold the smoke lock — §6). Add tests for new code
   (unit + a golden/determinism test for anything on the lockstep path). For
   player-visible features, also build `mac-release` and run a headless smoke;
   **flag visual-only verification as "needs in-window check" in the journal and
   press on** (you cannot drive interactive play autonomously).
7. **Land it.** Commit (imperative subject < 70 chars, **NO `Co-Authored-By`**),
   push to `nextgen/new-release`. **Watch CI** (`ci` + `determinism`) to green;
   if red, fix it **before** the next item. Never target `main`. Never
   force-push.
8. **Journal.** Update `AUTONOMOUS-ROADMAP.md`: mark done, record decisions +
   assumptions + sources, add discovered follow-ups, note anything needing
   eventual human/in-window/PC-Vulkan verification. Keep an elapsed-time stamp.
9. **Continue.** Schedule the next iteration. Repeat.

## 4. Decision-making & stuck policy

- **Never block on the user.** Editor/UX/design doubts → pick the Unity-like or
  Quake/BF-authentic default, document it, move on.
- **Stuck > ~2 attempts on one item?** Pivot: take a different roadmap item, or
  create a **new demo project** that needs the subsystem, and build it there.
  Don't grind.
- **Architecture is king.** Prefer clean lane boundaries, ECS-authoritative
  data, deterministic algorithms, no shortcuts that break a pillar. When two
  designs tie, pick the one that scales to thousands of entities + many players.
- **Use the codebase's grain:** vendor patterns already present; reuse
  `psy::gpu`, `scene::World`, `jobs`, `SpatialIndex`, `character_spine`, the net
  primitives, the render pipeline. Don't reinvent.
- **Assets:** the user has an AAA asset library but is unavailable to point at
  it. Use procedural/placeholder content (and the cooker/VFS) now; leave a
  clean import path for when the editor can pull real assets. Don't block on
  content.

## 5. Parallel-agent isolation (lesson learned — enforce it)

When fanning out Agent lanes (`isolation: "worktree"` or manual worktrees):
- Every lane-agent prompt MUST open with: run `pwd`; confirm it is under
  `.claude/worktrees/`; **NEVER `cd` to the main repo root**; edit only paths
  under the worktree; if a target path isn't under the worktree, STOP + report.
- And: `git fetch; git log --oneline -1 nextgen/new-release` must be current;
  `git reset --hard nextgen/new-release`; verify a known-recent file exists
  before working (stale-base guard).
- Disjoint directory ownership only; merge each back into `nextgen` with
  `--no-ff`; rebuild + full `ctest` after merges; resolve any non-exhaustive
  switch / EDITOR=OFF break the merge surfaces.
- Keep lanes to genuinely independent work; do the keystone/determinism-critical
  pieces yourself.

## 6. Build / test / run reference

- Build: `cmake --preset mac-debug && cmake --build --preset mac-debug`
- Test: hold the mac smoke lock around `ctest` (it runs headless sample bins):
  `LOCK=/tmp/psynder_gx_smoke.lockdir; while ! mkdir "$LOCK" 2>/dev/null; do sleep 1; done; trap 'rmdir "$LOCK"' EXIT; cd build/mac-debug && ctest`
- Player: `cmake --build --preset mac-release` then `scripts/run_release.sh`
  (interactive — autonomous can build + headless-smoke, not play).
- Headless smoke: `./build/mac-debug/bin/sample_02_crate --smoke-frames=N`
  (under the lock).
- CI triggers on `main`, `nextgen/**`, `integration/**`, `lane/**`. Keep
  `EDITOR=OFF` configs building (the player/sample skip themselves when the
  editor is off; samples are built in CI).

## 7. Non-negotiable rules (recap)

- No `Co-Authored-By` trailer. Imperative commit subjects < 70 chars.
- The agent OWNS CMake — never ask the user to edit CMake/presets. `tests/unit`
  GLOBs `*.cpp` (CONFIGURE_DEPENDS); new lane libs need a link line.
- Golden digests are build-config-specific — pin only the authoritative config
  (macOS/arm64/Release), cross-run determinism is the universal hard gate.
- ASCII-only Catch2 `TEST_CASE` names.
- `nextgen/new-release` is the trunk; promote to `main` only at the end via one
  PR (don't do it autonomously without the Definition of Done met + CI green).

## 8. Definition of Done

A **deterministic, networked, DOTS FPS prototype** that:
- runs server-authoritative with a predicted/reconciled client (the net loop
  driving real gameplay), lag-compensated hitreg;
- has a Quake3-class arena **and** a BF-light map, weapons + health + rounds +
  pickups, and crowd/bot AI playing the loop;
- renders through the unified `render::pipeline` (raster PBR-ish + post), with
  **minimal RT reflections/shadows behind a flag** done LAST;
- is authored in a **WYSIWYG editor** (ECS scene authoring, gizmos,
  play-in-editor, inspector, asset browser) with **PsyGraph** visual scripting
  driving gameplay;
- is **green on CI across macOS/Metal + Linux/Vulkan + Windows**, with the
  determinism matrix passing and a healthy golden suite.

When Done: write a final summary ADR + a `main` promotion PR (do not merge it),
and stop the loop. Until then: keep building.
