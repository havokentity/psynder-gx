# Editor Interaction Checklist

Manual verification after an editor/viewport miniwave.

## Start

1. Build and smoke:

   ```bash
   tools/smoke/editor_loop_smoke.sh
   ```

2. Launch `PsyArcadeGX`.
3. Open PsyEditorGX with the runtime console `web_console` command.
4. Confirm the in-game runtime console closes when PsyEditorGX opens.

## Coherence Sweep

Use this first. The detailed sections below are for debugging a failed sweep, not
for making every test pass feel like homework.

1. Create a fresh scene.
2. Add a camera, cube, sphere, and plane from the Hierarchy create menu.
3. Rename the cube from the Hierarchy.
4. Select cube and sphere together.
5. Duplicate the selection, then undo and redo the duplicate.
6. Delete the duplicated selection, then undo and redo the delete.
7. Select one object, edit its transform in Inspector, then undo and redo.
8. Switch `W`, `E`, and `R`; drag one translate, rotate, and scale handle.
9. Move the editor camera with RMB, MMB, wheel, and `F` frame if available.
10. Toggle render debug depth on and off.
11. Save, reload, and confirm selection, active camera, transforms, environment,
    and editor settings are coherent.

Pass:
- The engine viewport, Hierarchy, Inspector, console stats, and saved scene all
  describe the same state.
- Each committed user action creates one undo step.
- No web-only control lies about a feature that is not active in the engine.

## Vertical Slice Coherence Sweep

Use this after the editor shell sweep when a rendering/FPS wave lands. This is
the short test that says whether we are moving toward an authored FPS game, not
just a nicer editor.

1. Create or load a scene from the editor.
2. Add a camera, floor plane, two wall cubes, one target cube, and a player
   start or placeholder pawn if that feature exists.
3. Assign distinct solid materials to the floor, walls, and target.
4. Change time of day, exposure, and sun intensity.
5. Enable clouds, adjust coverage/density/speed, then let the scene run for a
   few seconds.
6. Press Play if play mode exists, move the pawn if available, then stop and
   return to edit mode.
7. Save, reload, and restart the engine.

Pass:
- Authored scene data, runtime rendering, and editor state describe the same
  world after save/reload/restart.
- Materials are visible in the live engine render path, not just stored in JSON.
- Sun/time-of-day/exposure/cloud edits visibly affect the live frame.
- Play mode, when present, uses the authored scene rather than a hardcoded
  sample fallback.
- Missing upcoming systems are reported as missing systems, not buried in a
  long UI checklist.

## Hierarchy Authoring Workflow

1. Open the Hierarchy panel.
2. Use the single create menu to add Camera, Cube, Sphere, and Plane.
3. Search or filter for `cube` if the panel exposes a search field.
4. Double-click or press `F2` on the cube and rename it.
5. Confirm duplicate names resolve to a unique entity name.
6. Shift-select a range and Cmd/Ctrl-toggle a second selection.
7. Duplicate the selected set.
8. Delete the duplicated set.
9. Set a camera row active.
10. Clear selection from the hierarchy action bar or empty space.

Pass:
- Create, rename, duplicate, delete, active camera, and clear-selection actions
  all route through the scene commit/history path.
- The hierarchy remains scrollable from top to bottom while action bars are
  visible.
- Search/filter, when present, never changes selection by itself.

## Selection Inspector Transaction Workflow

1. Select a single entity in the live viewport.
2. Confirm the Hierarchy row and Inspector target update.
3. Edit transform values in Inspector and commit with Enter or blur.
4. Undo and redo the Inspector edit.
5. Select multiple entities in Hierarchy.
6. Confirm Inspector reports a multiselect summary and common components.
7. Use a gizmo drag on one selected entity, then undo and redo.
8. Clear selection from the viewport or hierarchy.

Pass:
- Single selection and multiselect payloads are explicit and never flicker
  through stale unsupported states.
- Inspector commits are transactional.
- Clearing selection clears viewport highlight, Hierarchy, Inspector, and
  console selected-entity stats together.

## Camera Gizmo Render Debug Workflow

1. Move the editor camera with RMB look, MMB pan, wheel dolly, and `F` frame if
   available.
2. Save and reload the scene.
3. Confirm the editor viewport camera returns without changing the active game
   camera.
4. Switch translate, rotate, and scale with `W`, `E`, and `R`.
5. Drag one axis handle and one plane handle where applicable.
6. Toggle `render_debug depth` from console or editor UI.
7. Toggle back to `render_debug off`.
8. Overlap two primitives and confirm depth behavior remains correct.

Pass:
- Camera controls do not run while runtime console text input is focused.
- Gizmo visuals and hit targets are engine-rendered.
- Render debug state converges between console, editor UI, runtime stats, and
  saved editor settings.

## Object Select And Clear

1. In PsyEditorGX, add a cube, sphere, and plane.
2. Left-click the cube in the live engine viewport.
3. Confirm the cube highlights in the engine viewport.
4. Confirm Hierarchy selects the cube.
5. Confirm Inspector shows the cube transform.
6. Left-click empty viewport space.
7. Confirm the engine highlight clears.
8. Confirm Hierarchy and Inspector clear selection.

Pass:
- Engine highlight, Hierarchy selection, and Inspector selection always match.
- Empty-space click clears all three.

## Inspector Sync

1. Select the sphere in the live engine viewport.
2. Rename it in Inspector.
3. Confirm the Hierarchy row updates without losing selection.
4. Edit position values in Inspector.
5. Confirm the live engine object moves.
6. Select another object, then select the sphere again.
7. Confirm Inspector still shows the latest name and transform.

Pass:
- Inspector edits update the engine.
- Engine or Hierarchy reselection returns the same values.
- No stale fields remain after rename, remove, or cleared selection.

## Real Gizmo Drag

1. Select the cube.
2. Confirm real in-engine translate handles appear at the cube.
3. Drag red X handle.
4. Drag green Y handle.
5. Drag blue Z handle.
6. Confirm each drag moves only along the expected axis.
7. Drag each plane handle when available:
   - XY moves in X and Y only.
   - XZ moves in X and Z only.
   - YZ moves in Y and Z only.
8. Confirm dragging a handle does not accidentally reselect the object behind it.

Pass:
- Axis handles have visible arrowheads.
- Plane handles move on the intended plane.
- Drag starts without a position jump.
- Selected object remains selected during and after drag.

## Real Depth

1. Place the cube partly behind the sphere from the current camera angle.
2. Select the cube.
3. Orbit or RMB-look until the sphere is between the camera and part of the cube.
4. Confirm the cube highlight and translate handles depth-test against the sphere.
5. Move the camera so the cube is in front of the sphere.
6. Confirm the same highlight and handles are fully visible again.
7. Repeat with the placement grid behind the selected object.

Pass:
- Selection highlight respects real scene depth.
- Gizmo handles are not drawn as a flat web overlay on top of all geometry.
- The grid never occludes selected geometry or gizmo handles incorrectly.
- Depth behavior is stable while moving the camera.

## Gizmo Overlay

1. Select the cube and open the Hierarchy, Inspector, Console, and Profiler panels.
2. Confirm the gizmo is rendered in the live engine viewport, not as HTML controls.
3. Resize the editor window and move docked panels around the viewport.
4. Confirm the gizmo remains anchored to the selected object's world position.
5. Open the runtime console.
6. Confirm the gizmo does not accept drags while console input is focused.
7. Close the console and drag a gizmo handle.

Pass:
- No fake web axis/plane buttons are visible.
- Gizmo location tracks the engine camera and selected entity after resize.
- Docked web panels do not cover or offset engine hit-testing.
- Console focus blocks editor viewport manipulation until focus returns.

## Viewport Camera Persistence

1. Navigate the editor viewport camera with RMB look, `WASD`, pan, dolly, and `F` frame.
2. Save the scene.
3. Close PsyEditorGX.
4. Reopen PsyEditorGX from the runtime console.
5. Confirm the editor viewport camera returns to the saved view.
6. Load the saved scene from disk.
7. Confirm active scene camera data is unchanged unless explicitly edited.
8. Restart `PsyArcadeGX` and open PsyEditorGX again.

Pass:
- Editor viewport camera position, orientation, and framing survive editor close/reopen.
- Saved scene camera components survive save/load/restart.
- Editor viewport camera persistence does not overwrite the active game camera.
- Missing or older camera metadata falls back to a usable default view.

## Transform Modes W E R

1. Select the cube.
2. Press `W` and confirm the live engine gizmo switches to translate.
3. Press `E` and confirm the live engine gizmo switches to rotate.
4. Press `R` and confirm the live engine gizmo switches to scale.
5. Switch the viewport gizmo from translate to rotate.
6. Drag X, Y, and Z rotation rings.
7. Confirm the Inspector `rotation_euler_deg` values update on release.
8. Undo and redo one rotation drag.
9. Switch the viewport gizmo from rotate to scale.
10. Drag X, Y, and Z scale handles.
11. Confirm the Inspector `scale` values update on release.
12. Try uniform scale if the viewport exposes a center handle.
13. Save, reload, and confirm rotation and scale values persist.

Pass:
- `W`, `E`, and `R` select translate, rotate, and scale without stealing text-field focus.
- Rotate mode changes orientation only, without position drift.
- Scale mode changes scale only, without position or rotation drift.
- Rotation and scale drags each commit one undo step on release.
- Negative, zero, or near-zero scale is clamped or rejected in a visible way.

## Rotate Scale Visual Gizmos

1. Select the cube and switch to rotate with `E`.
2. Confirm rotation handles are visually distinct from translate arrows.
3. Drag X, Y, and Z rotate handles from several camera angles.
4. Confirm the active axis highlights while dragging and returns to idle after release.
5. Switch to scale with `R`.
6. Confirm scale handles are visually distinct from translate arrows and rotate rings.
7. Drag X, Y, and Z scale handles from several camera angles.
8. Confirm hover, active, and disabled states remain readable over light and dark clear colors.

Pass:
- Rotate and scale are real engine-rendered gizmos, not web-side controls.
- Rotate handles communicate axis/orientation without looking like translation arrows.
- Scale handles communicate axis/uniform intent without hiding object selection.
- Hover and active states survive resize, depth view, grid toggle, and console focus changes.

## Plane Translation Handles

1. Select the cube.
2. Switch to translate mode with `W`.
3. Drag the XY plane handle.
4. Confirm movement changes X and Y only.
5. Drag the XZ plane handle.
6. Confirm movement changes X and Z only.
7. Drag the YZ plane handle.
8. Confirm movement changes Y and Z only.
9. Orbit the editor camera and repeat each plane drag from a steep angle.
10. Drag near an object behind the plane handle.

Pass:
- Plane handles are real engine-rendered handles, not HTML buttons.
- Plane movement is constrained to the selected plane.
- Dragging starts without a visible jump.
- Plane handle hit-testing wins over objects behind it.

## Multiselect

1. Select the cube.
2. Add the sphere to selection with the platform multiselect modifier.
3. Add the plane to selection.
4. Confirm Hierarchy shows all selected rows.
5. Confirm Inspector shows shared transform controls or a clear mixed-value state.
6. Move the selection with the translate gizmo.
7. Undo and redo the group move.
8. Remove one object from selection with the same modifier.
9. Click empty viewport space.

Pass:
- Multiselect state is identical in engine viewport, Hierarchy, and Inspector.
- Group translate preserves each object's relative offset.
- Undo/redo restores the exact selection and transforms.
- Empty-space click clears the whole selection.

## Multiselect Delete Duplicate

1. Select a cube, sphere, and plane with Shift range selection.
2. Delete the selected set.
3. Undo and confirm all deleted entities return with names, transforms, and active camera state intact.
4. Redo and confirm the same selected set is removed again.
5. Select two primitives with Cmd/Ctrl additive selection.
6. Duplicate the selected set.
7. Confirm duplicate names are unique and each duplicate preserves relative transform offsets.
8. Undo and redo the duplicate operation.
9. Try deleting or duplicating a selection that includes the active camera.

Pass:
- Delete and duplicate operate on the full hierarchy selection, not only the primary selected entity.
- Active camera is cleared or remapped intentionally when its entity is deleted or duplicated.
- Undo/redo restores selection membership along with entity data.
- Inspector shows a mixed or multi-selection state instead of stale single-entity fields.

## Scene Dirty UX

1. Create or load a scene and confirm the dirty indicator reads clean.
2. Rename an entity.
3. Confirm the dirty indicator changes to dirty before saving.
4. Save the scene.
5. Confirm the dirty indicator returns to clean.
6. Make changes through each path:
   - gizmo drag
   - Inspector transform edit
   - environment edit
   - JSON text edit
   - add/remove entity
7. For each path, confirm dirty status changes exactly once per committed edit.
8. Try closing or quitting with unsaved changes.

Pass:
- Dirty state is visible in the scene UI without needing dev tools.
- Save clears dirty only after the engine reports success.
- Failed save keeps the scene dirty and shows a useful message.
- Close/quit with unsaved changes warns or clearly preserves recoverable state.

## Scene Authoring Migration

1. Create a new loose scene from PsyEditorGX.
2. Add a camera, cube, sphere, and plane.
3. Save to `projects/PsyArcadeGX/scenes/startup.scene.bin`.
4. Confirm `projects/PsyArcadeGX/manifest.psycooked` points at the saved scene.
5. Restart `PsyArcadeGX`.
6. Confirm startup loads the saved scene without hand-authored sample fallback data.
7. Load an older loose scene file if one is available.
8. Save it again.

Pass:
- Scene load/save flows through the engine scene_authoring path.
- The migrated file keeps entity names, transforms, renderable flags, and camera data.
- Older scene data either migrates losslessly or reports a clear incompatibility.
- The web panel is a client of engine-owned scene state, not the persistence authority.

## Active Camera Authoring

1. Create a scene with no camera and confirm the engine shows the no-camera rendering state.
2. Add a camera from the editor.
3. Confirm the new camera becomes the active camera.
4. Add a second camera.
5. Set the second camera active.
6. Save, reload, and restart `PsyArcadeGX`.
7. Confirm the chosen active camera is still active.
8. Delete the active camera.
9. Confirm the scene enters a deliberate no-active-camera state or selects a replacement camera visibly.

Pass:
- Active camera changes are explicit authoring operations and are saved through scene_authoring.
- The editor viewport camera remains separate from the active game camera.
- Hierarchy, Scene settings, and runtime render state agree on the active camera.
- No-camera rendering is stable and visually distinct from the boot/intro VFX state.

## Undo Redo Transactions

1. Select a cube.
2. Move it with a real engine gizmo drag.
3. Press `Cmd+Z` on macOS or `Ctrl+Z` on Windows/Linux.
4. Confirm the cube returns to its previous position.
5. Press `Cmd+Shift+Z` / `Ctrl+Shift+Z`, or `Ctrl+Y`.
6. Confirm the cube returns to the dragged position.
7. Repeat with:
   - add object
   - remove object
   - rename object
   - Inspector transform edit
   - environment color edit

Pass:
- Each user action creates one undo step.
- Gizmo drags commit one undo step on release, not one step per mouse move.
- Undo/redo updates engine view, Hierarchy, and Inspector together.
- Create, delete, transform, environment, and selection-affecting edits register through the same transaction path.

## Render Debug And Depth View

1. Open the editor render debug controls.
2. Toggle depth view.
3. Confirm the live engine viewport shows depth in a readable debug visualization.
4. Toggle back to lit/color view.
5. Toggle bounds or selection debug if present.
6. Move an object behind another object.
7. Confirm the depth debug view changes as occlusion changes.
8. Save, reload, and confirm debug visualization state either persists intentionally or resets intentionally.

Pass:
- Depth view is driven by the engine render path, not by a fake web screenshot effect.
- Debug toggles do not change saved game scene data unless explicitly marked as editor settings.
- Returning to color view restores the normal scene without stale debug state.
- Debug overlays do not break gizmo hit-testing or console input focus.

## Render Debug Controls

1. Open the editor render debug controls.
2. Toggle `off`, then `depth`, then `off` again.
3. Confirm the runtime console `render_debug` state matches the editor control.
4. Use the runtime console `render_debug depth` command.
5. Confirm the editor control updates to the same state.
6. Use the runtime console `depth_view` shortcut.
7. Confirm the editor control updates without needing a page refresh.
8. Save, reload, and confirm the chosen persistence behavior is intentional.

Pass:
- Render debug controls drive engine state through editor settings, not local web-only state.
- Console commands and editor controls converge on one render_debug value.
- Debug UI stays usable while hierarchy and Inspector panels are open.
- Debug visualization does not dirty game-scene data unless intentionally stored as editor settings.

## Environment Panel

1. Open the Scene settings panel.
2. Change clear color with the color picker.
3. Confirm the live engine viewport updates immediately.
4. Change individual RGBA channels.
5. Confirm invalid values clamp or reject visibly.
6. Change sky mode if exposed.
7. Undo and redo an environment edit.
8. Save, reload, and restart `PsyArcadeGX`.

Pass:
- Environment edits are live, transactional, and saved through scene_authoring.
- Clear color and sky mode round-trip through the saved scene.
- Environment edits mark the scene dirty exactly once per committed edit.
- No Inspector or Hierarchy selection is required to edit scene environment.

## Solid Material Authoring

1. Select a cube in the Hierarchy.
2. Open the material controls in Inspector or Scene authoring.
3. Assign or edit albedo, roughness, metallic, emissive color, and emissive
   intensity if exposed.
4. Duplicate the cube.
5. Give the duplicate a different material.
6. Undo and redo one material edit.
7. Save and reload the scene.

Pass:
- Material changes are authored on the selected entity or referenced material,
  not only in local web state.
- The editor distinguishes shared material references from per-object override
  values when that feature lands.
- Undo/redo and save/load preserve material assignments.
- Invalid scalar/color values clamp or reject visibly.

## Material Runtime Rendering

1. Place two cubes side by side with different material colors.
2. Set one material rough/low-metallic and another shiny/high-metallic when
   those controls exist.
3. Move the sun/time-of-day so lighting changes across both objects.
4. Toggle render debug depth and return to color rendering.
5. Save/reload and confirm the same material differences remain.

Pass:
- Runtime geometry uses authored material data in the engine render path.
- Material color/roughness/metallic/emissive behavior is visible without
  restarting or hand-editing launch parameters.
- Debug views do not destroy normal material state when disabled.
- The old hardcoded primitive tint path is not the authority for authored
  scene materials.

## Sun Time Of Day

1. Open Scene settings.
2. Scrub or edit time of day from morning to noon to evening.
3. Change sun color and intensity.
4. Confirm active camera rendering and editor viewport rendering agree.
5. Save/reload/restart.

Pass:
- Sun direction/color/intensity derive from environment settings.
- Time-of-day changes are live and transactional.
- Restarting the engine restores the saved lighting state.
- Explicit no-camera scenes still show the no-camera state while preserving
  authored environment data.

## Raymarched Clouds

1. Enable cloud rendering from Scene settings when available.
2. Adjust coverage, density, speed, height, and thickness.
3. Let time advance and confirm wind/time animation is visible.
4. Move time of day and confirm clouds respond to sun color/direction.
5. Toggle clouds off and on.

Pass:
- Clouds render through the engine GPU path, not a web canvas overlay.
- Cloud controls affect the live frame without recompiling shaders.
- Performance remains stable enough for editor iteration.
- Cloud params save/reload with the scene environment.

## Exposure Tonemapping

1. Put bright and dark objects in the same scene.
2. Increase exposure, then decrease exposure.
3. Change sun intensity and confirm exposure still behaves predictably.
4. Save/reload/restart.

Pass:
- Exposure is part of scene/environment rendering state.
- Exposure changes are live, undoable, and saved intentionally.
- Bright values do not blow out the editor UI overlays.
- Depth/debug views and normal color view converge after toggling.

## Scene Runtime Entities

1. Author a scene with primitives, a camera, materials, and environment data.
2. Save the scene.
3. Restart the engine and load the scene without manual launch parameters.
4. Select and transform an entity.
5. Confirm runtime stats/entity labels update.

Pass:
- Runtime objects are produced from scene_authoring data, not hardcoded sample
  fallback objects.
- Entity names, components, transforms, materials, active camera, and
  environment data round-trip together.
- Runtime selection and Inspector selection refer to the same entity identity.

## FPS Pawn Play Mode

1. Add or select a player start when play mode exists.
2. Press Play from the editor.
3. Confirm the pawn spawns at the authored player start.
4. Move, mouse-look, jump/crouch/sprint if available, and collide with the
   authored floor/walls.
5. Fire the prototype weapon if available.
6. Press Stop and confirm edit mode restores the authored scene state.

Pass:
- Play mode uses the authored scene and current active camera/player start
  contract.
- Physics/collision is real enough for an FPS blockout, not visual-only motion.
- Stop returns to editor state without permanently dirtying transforms unless
  explicitly requested.
- Missing pawn/weapon features are tracked as vertical-slice gaps, not editor
  UI regressions.

## Player Start Authoring

1. Add a PlayerStart entity from the authoring UI when exposed.
2. Move it to a deliberate spawn point above the authored floor.
3. Save and reload the scene.
4. Restart the engine and open the same scene from the editor.
5. Press Play.

Pass:
- PlayerStart is stored as scene authoring data, not a hardcoded runtime spawn.
- Save/reload/restart preserves the PlayerStart transform.
- Play mode spawns from PlayerStart when one exists and uses a visible fallback
  only when no PlayerStart exists.

## Play Stop Runtime Commands

1. Open the runtime console.
2. Run `play`.
3. Confirm the engine enters play mode.
4. Run `stop`.
5. Confirm the engine returns to edit mode.
6. Repeat from the editor Play/Stop control if exposed.

Pass:
- `play` and `stop` are runtime console commands, not only web buttons.
- Repeated `play` or `stop` calls are idempotent and report the current state.
- Entering and leaving play mode does not corrupt the authored scene document.

## Character Spine Runtime Integration

1. Author a floor, two wall cubes, a target cube, and a PlayerStart.
2. Press Play.
3. Move forward, backward, strafe, jump if available, crouch if available, and
   collide with the authored floor/walls.
4. Stop, then move objects in edit mode and Play again.

Pass:
- Play-mode motion uses `physics::character_spine`, not visual-only camera
  drift.
- Static primitive colliders are built from authored scene entities.
- Stop destroys the transient play world and edit mode remains responsive.

## Runtime Play State Stats

1. Open the runtime console.
2. Run `render_stats` or view the stats panel.
3. Press Play.
4. Confirm stats report play state, spawn source, and pawn/collider counts when
   available.
5. Press Stop.
6. Confirm stats return to edit state.

Pass:
- Runtime stats include a stable play-state line for smoke/debugging.
- Stats update live without refreshing PsyEditorGX.
- Missing deeper FPS features are visible as counts or state fields, not hidden
  behind generic "scene rendering" text.

## Save Load Persistence Matrix

1. Create a scene with a camera, one solid-material cube, one floor blockout
   helper, and one FPS Pawn or PlayerController entity.
2. Select the cube, set a distinctive material, and change environment clear
   color, sun/time of day, exposure, and cloud coverage.
3. Select the FPS Pawn and change walk speed, run speed, jump velocity, mouse
   sensitivity, capsule radius, and capsule height.
4. Save the scene, reload it from the editor, then restart the engine and load
   the same scene again.
5. Confirm the selected entity, hierarchy selection, active camera, material,
   environment, and PlayerController values all match the saved state.

Pass:
- PlayerController, material, environment, selection, and active camera data
  round-trip through scene_authoring serialization, not web-only state.
- Reloading does not invent a different startup scene or silently drop the
  authored FPS Pawn.
- Invalid or older saved values sanitize visibly and keep the editor usable.

## FPS Pawn Tuning

1. Select the PlayerStart or player controller settings when exposed.
2. Change walk speed, run speed, jump height or velocity, crouch height, and
   mouse/look sensitivity.
3. Press Play and move through an authored floor/wall blockout.
4. Stop, tweak one value, and Play again without restarting the engine.
5. Save, reload, and confirm the tuning values persist.

Pass:
- Pawn tuning is authored scene/editor data, not hardcoded sample constants.
- Play mode applies tuning on enter and after save/reload.
- Extreme values clamp or reject visibly instead of producing unusable motion.
- Tuning edits create normal undo/redo transactions where the UI exposes them.

## Formal Play Input Capture State

1. Press Play with the runtime console closed.
2. Run or inspect `input_capture` and confirm it reports `captured` while the
   pawn owns relative mouse/look input.
3. Move the mouse without holding RMB and confirm the pawn camera looks around.
4. Open the runtime console and type into it.
5. Confirm input capture releases or is blocked so text input does not move the
   pawn.
6. Close the console, run `input_capture on` if needed, then Stop play mode.

Pass:
- Play mode has an explicit capture state or command; it is not inferred from
  hidden button state.
- Console focus, editor text fields, and modal UI always win over game input.
- Leaving play mode releases capture and returns RMB/MMB/wheel to editor camera
  navigation.

## Play Mode Input Capture

1. Press Play from PsyEditorGX.
2. Confirm mouse look uses relative movement or equivalent capture while in play
   mode and does not require holding RMB.
3. Confirm `WASD`, sprint, crouch, and jump route to the pawn while playing.
4. Open the runtime console and confirm typing does not move the pawn or camera.
5. Close the console and confirm play input resumes.
6. Press Stop and confirm editor camera controls return to RMB/MMB/wheel behavior.

Pass:
- Play-mode input capture is explicit and visible in runtime state or stats.
- Text input, console focus, and editor fields always win over game controls.
- Leaving play mode releases capture and restores editor navigation.
- Mouse sensitivity is stable across restart and scene reload when authored.

## Collision Debug Overlay Stats

1. Author a floor, walls, target cube, and PlayerStart.
2. Press Play and collide with each surface.
3. Toggle the collision debug overlay or stats command when exposed.
4. Confirm player capsule/body and static colliders are visible or counted.
5. Move a wall in edit mode, Play again, and confirm debug data updates.

Pass:
- Collision debug uses runtime physics data, not a separate web approximation.
- Collider counts distinguish player, static scene, and optional trigger data.
- Debug overlays do not break depth, gizmo hit-testing, or text input focus.
- Missing overlay rendering still reports useful collision stats.

## Blockout Primitive Helpers

1. Open the Hierarchy create menu.
2. Add floor, wall, ramp, cover, and crate/blockout helpers when available.
3. Confirm each helper creates normal scene entities with editable transforms.
4. Rename, duplicate, delete, undo, and redo helper-created entities.
5. Save and reload the scene.

Pass:
- Blockout helpers are convenience authoring commands over normal primitives.
- Helpers choose useful dimensions/materials without creating hidden special
  cases.
- Helper entities appear in Hierarchy, Inspector, runtime render, and physics
  collision consistently.
- No new sample app is needed to prove blockout creation.

## FPS Blockout Authoring

1. Use the fast FPS blockout controls to add floor, wall, block, ramp, and
   cover pieces.
2. Confirm each helper creates a normal primitive entity with Transform,
   Renderable, material, and physics collision behavior in play mode.
3. Rename each helper-created entity and select it from the live viewport and
   Hierarchy.
4. Duplicate and delete a helper-created set, then undo and redo both actions.
5. Save, reload, and press Play to collide against the same authored layout.

Pass:
- Blockout helpers are authoring presets over normal scene entities, not a
  separate sample path or one-off runtime fallback.
- Helper dimensions and material defaults are useful for FPS iteration.
- Depth, selection, collision, and save/load behavior are identical to manually
  created primitives.

## Hierarchy Selection Sync

1. Select an entity from the live viewport.
2. Confirm Hierarchy and Inspector update immediately.
3. Select an entity from Hierarchy.
4. Confirm the live viewport highlight and Inspector update immediately.
5. Shift/Cmd/Ctrl select multiple rows in Hierarchy.
6. Click empty viewport space.

Pass:
- Viewport, Hierarchy, Inspector, and runtime stats describe the same selection.
- Multiselect does not flicker through stale single-selection fields.
- Empty-space click clears selection everywhere.
- Selection sync works while panels are scrolled and action bars are visible.

## Quit Close Editor

1. Open PsyEditorGX from the runtime console.
2. Click the quit/close-engine control in PsyEditorGX.
3. Confirm the engine app closes.
4. Confirm the PsyEditorGX window closes too.
5. Relaunch the engine normally and open PsyEditorGX again.

Pass:
- Quit does not require the dev server path.
- The editor does not remain connected to a dead engine.
- Unsaved changes are warned about or remain recoverable.
- Relaunch returns to the last valid scene/editor state intentionally.

## Typed Scene Authoring Settings

1. Save a scene containing grid visibility, snap mode, transform mode, render debug mode, and viewport camera.
2. Reload the scene through the editor.
3. Confirm every setting is parsed through typed scene_authoring helpers.
4. Manually corrupt one setting token in a copy of the scene file.
5. Load it and confirm the value sanitizes or reports a clear incompatibility.
6. Save again.

Pass:
- Scene settings use typed helpers for transform mode, render debug mode, snap, and viewport camera.
- Unknown JSON fields are preserved when these helpers patch editor settings.
- Invalid FOV, snap step, colors, and enum tokens clamp or fall back predictably.
- Runtime scene view derives from scene_authoring state rather than duplicating parsing logic in the web UI.

## RMB Camera

1. Focus the engine viewport.
2. Hold right mouse button and drag left/right.
3. Confirm horizontal look direction feels correct.
4. Hold right mouse button and drag up/down.
5. Confirm vertical look direction feels correct.
6. While holding RMB, press `WASD`.
7. Confirm camera moves relative to its current orientation.
8. Try middle mouse drag pan.
9. Try mouse-wheel dolly.
10. Press `F` with an object selected.

Pass:
- RMB look, MMB pan, wheel dolly, and `F` frame all respond.
- Wheel movement is controllable, not jumpy.
- Camera tools do nothing while the runtime console is open.

## Grid Toggle

1. Toggle the editor grid off.
2. Confirm the live engine placement grid disappears.
3. Toggle the editor grid on.
4. Confirm the live engine placement grid returns.
5. Select and clear objects while toggling the grid.

Pass:
- Grid visibility is independent from object selection.
- Grid remains transparent and does not read as a solid floor.

## Quit

1. Click the PsyEditorGX quit button.
2. Confirm the engine app closes.
3. Confirm the PsyEditorGX window closes.

Pass:
- No dev server is required.
- No editor window remains after the engine closes.

## No New Samples Guard

1. Run `tools/smoke/editor_loop_smoke.sh`.
2. Confirm the smoke script accepts only the maintained sample directories:
   - `samples/00_clear`
   - `samples/01_triangle`
   - `samples/02_crate`
3. Create a temporary extra sample directory outside a commit if you need to validate the guard.
4. Confirm the guard fails on the extra directory.
5. Remove the temporary directory and rerun smoke.

Pass:
- New editor/engine capabilities are proven through the existing maintained samples.
- No extra sample app is added to avoid maintenance spread.
- The guard is based on the working tree, so it catches untracked sample directories too.
