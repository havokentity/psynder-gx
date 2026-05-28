# CLAUDE.md — session handoff (Psynder-GX)

> Working snapshot for picking up in a fresh Claude session (terminal or a new
> GUI window). Read **`AGENTS.md`** first (lane ownership + hard rules) and
> **`docs/adr/ADR-019-hybrid-jolt-dots-physics.md`** (physics architecture).
> Broader audit: `docs/review/ARCH-REVIEW-2026-05-27.md`.

## Current state
- **Branch:** `codex/miniwave-dots-crate` (was clean at handoff).
- **Build:** `cmake --preset mac-debug && cmake --build --preset mac-debug`
- **Test:** `cd build/mac-debug && ctest` — **518 passing**, keep green.
- **Run the playable window:** `scripts/run_release.sh` → in the window press `` ` ``/`~` to open the console → type `play` → `` ` `` to close console → **WASD + mouse-look + Space**; **left-click** to shoot crates. (Headless: `./build/mac-debug/bin/sample_02_crate --smoke-frames=N`, under the smoke lock — see AGENTS.md.)

## What landed this session (see `git log --oneline -14`)
- **Collision-ghost fix:** shooting a crate now removes its Jolt static body, not just the ECS entity, so no invisible collider lingers. `character_spine::add_static_box/sphere` return a stable `StaticBodyHandle`; new `remove_static_body(World*, handle)` (`CharacterSpine.{h,cpp}`). `build_jolt_statics_from_ecs` returns the entity→handle map (`EcsCharacterBridge.h`); `PlayModeState` keeps it and the hitscan-destroy path removes the matching body (`samples/02_crate/main.cpp`). Covered by a new unit test (capsule walks through where the wall was). Verified in the window.
- **Scene-on-ECS migration steps 3–5:** `render/pipeline/Extract.cpp` walks the ECS (`for_each_chunk<TransformWS,Collider,RenderMaterial>`) with builtin meshes (`BuiltinMeshes.cpp`); `02_crate` play mode runs on `scene::World`.
- **Player on Jolt via ECS:** `engine/physics/core/EcsCharacterBridge.h` projects ECS `Collider` entities into a Jolt world (`character_spine`); the `CharacterVirtual` solves and writes back to the pawn's `TransformWS`. The old hand-rolled AABB resolver was removed; `CharacterController.h` is now just the player's ECS components.
- **FPS play mode:** macOS mouse capture (`set_mouse_captured` in `MacosPlatform.mm`, NSEvent-delta look), strafe fix, Esc no longer quits in play mode, editor gizmo/grid/markers hidden while playing.
- **Combat in the window:** `samples/combat/Combat.h` converged onto the canonical `scene::Collider`/`TransformWS`; left-click hitscan destroys the hit crate.
- **ADR-019:** hybrid physics — Jolt for player + rigid/ragdoll/convex authoritative bodies; custom DOTS for mass homogeneous agents; `scene::World` authoritative (every mover writes `TransformWS`). PhysX and a full custom rigid-body engine were considered and rejected.

## Open threads / next steps
1. **Jolt rigid-body enemies (top of list)** (ADR-019 class 2): pushable/ragdoll enemies as Jolt bodies built from ECS, synced back. The static-body lifecycle now exists (`add_static_*` → `StaticBodyHandle` → `remove_static_body`); the dynamic-body equivalent (`create_body` with mass > 0, transform written back to `TransformWS` each tick) is the next layer on `character_spine`.
2. **DOTS mass-agent system** (ADR-019 class 1): steering/avoidance on the `UniformGrid`/`Bvh` broadphase + golden determinism test.
3. **Pin Jolt by commit SHA** (ARCH review B3) — determinism-critical now that lockstep rides on Jolt; it's fetched by mutable git tag today.
4. **Deferred debt:** editor authoring still uses `ScenePrimitive` (review A7); the sample draws via its own `scene_pipeline`, not `render::pipeline::render()` (step-3 extract is built+tested but not adopted in the sample).

## Conventions (also in AGENTS.md)
- **No `Co-Authored-By` trailer** in commits.
- The agent owns CMake (never ask the user to edit CMakeLists/presets).
- Watch CI from the first push (libc++/MSVC STL + platform-define gaps surface only on CI).
- 1 world unit = 1 metre; real physics constants; determinism is a hard pillar.
