# Editor Loop Smoke

Lightweight verification for the current PsyArcadeGX editor loop after a miniwave.

Run from the repository root:

```bash
tools/smoke/editor_loop_smoke.sh
```

The harness checks:

- `npm --prefix engine/editor/web run build`
- `cmake --build --preset mac-release --target PsyArcadeGX`
- `cmake --build --preset mac-release --target sample_02_crate`
- fake web translate/snap gizmo controls are absent from `SceneView.tsx`
- the manual checklist still carries the high-value coherence workflow sections
- grouped static workflow contracts for hierarchy create/rename/delete/duplicate,
  multiselect and selection sync, Inspector single/multi payloads, undo/redo
  transactions, camera controls and viewport persistence, gizmo modes, scene
  robustness, environment editing, render_debug convergence, render depth, and
  vertical-slice rendering/FPS readiness
- play-mode vertical-slice anchors for PlayerStart authoring, `play`/`stop`
  runtime console commands, character-spine integration, and runtime stats that
  report the current play state
- hard anchors for FPS pawn tuning, formal play-mode input capture state,
  collision debug overlay or stats, FPS blockout primitive helpers, hierarchy
  selection sync, quit/close-editor behavior, and save/load persistence of
  PlayerController, material, environment, active camera, and selection state
- a no-new-samples guard that fails on unexpected untracked sample directories
- serialized Mac runtime smoke through `scripts/smoke_sample.sh`

The manual editor interaction checklist lives at:

```bash
tools/smoke/editor_interaction_checklist.md
```

Start with the coherence sweep instead of walking every micro-test. The detailed
sections are there to isolate failures after the sweep catches a workflow problem.
It covers hierarchy authoring, selection/Inspector/transaction sync, camera and
gizmo behavior, render debug, real depth, active-camera authoring, scene dirty
UX, scene_authoring migration, typed settings helpers, environment editing, grid
toggle, no-new-samples guard, and quit behavior. It now also carries a vertical
slice sweep for serious FPS/rendering progress: solid material authoring,
runtime material rendering, sun/time-of-day, raymarched cloud parameters,
exposure/tonemapping, scene runtime entities, PlayerStart authoring, play/stop
commands, character-spine movement, runtime play-state stats, the FPS
pawn/play-mode loop, the save/load persistence matrix, formal input capture,
collision debug/stats, FPS blockout authoring, hierarchy selection sync, and
quit/close-editor expectations.

The vertical-slice grep contract is intentionally split. Stable protocol,
runtime-scene, material/environment, and first playable-mode anchors are hard
requirements. Deeper future features, such as weapon behavior and richer
controller affordances, stay in the manual checklist until their systems land.

Useful overrides:

```bash
PSYNDER_GX_PRESET=mac-release tools/smoke/editor_loop_smoke.sh
PSYNDER_GX_SMOKE_FRAMES=30 tools/smoke/editor_loop_smoke.sh
PSYNDER_GX_SKIP_RUNTIME_SMOKE=1 tools/smoke/editor_loop_smoke.sh
PSYNDER_GX_SKIP_GREP_CHECKS=1 tools/smoke/editor_loop_smoke.sh
PSYNDER_GX_PRINT_MANUAL_CHECKLIST=1 tools/smoke/editor_loop_smoke.sh
```
