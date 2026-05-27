#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Verify the current PsyArcadeGX editor loop after a miniwave.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)

PRESET=${PSYNDER_GX_PRESET:-mac-release}
BUILD_DIR=${PSYNDER_GX_BUILD_DIR:-"$ROOT/build/$PRESET"}
SMOKE_FRAMES=${PSYNDER_GX_SMOKE_FRAMES:-10}
SKIP_WEB_BUILD=${PSYNDER_GX_SKIP_WEB_BUILD:-0}
SKIP_BUILDS=${PSYNDER_GX_SKIP_BUILDS:-0}
SKIP_RUNTIME_SMOKE=${PSYNDER_GX_SKIP_RUNTIME_SMOKE:-0}
SKIP_GREP_CHECKS=${PSYNDER_GX_SKIP_GREP_CHECKS:-0}
PRINT_MANUAL_CHECKLIST=${PSYNDER_GX_PRINT_MANUAL_CHECKLIST:-0}

WEB_DIR="$ROOT/engine/editor/web"
PSY_ARCADE_BIN="$BUILD_DIR/bin/PsyArcadeGX"
SAMPLE_BIN="$BUILD_DIR/bin/sample_02_crate"
MANUAL_CHECKLIST="$SCRIPT_DIR/editor_interaction_checklist.md"

log() {
    printf '[editor-loop-smoke] %s\n' "$*"
}

run() {
    log "running: $*"
    "$@"
}

check_fake_web_gizmo_absent() {
    local scene_view="$ROOT/engine/editor/web/src/panels/SceneView.tsx"
    local forbidden_actions
    local forbidden_ui

    forbidden_actions='(action|type):[[:space:]]*["'\''](move_selected_entity|axis_nudge|plane_nudge)["'\'']'
    forbidden_ui='(className=[^[:space:]]*["'\'']psy-scene-transform-tools|>[[:space:]]*(axis|plane)[[:space:]]+move[[:space:]]*<)'

    log "checking fake web gizmo controls are absent"
    if rg -n --pcre2 "$forbidden_actions" "$scene_view"; then
        log "forbidden fake web gizmo command wiring found in $scene_view"
        return 1
    fi
    if rg -n --pcre2 "$forbidden_ui" "$scene_view"; then
        log "forbidden fake web gizmo UI found in $scene_view"
        return 1
    fi
}

check_manual_checklist_coverage() {
    local required_headings=(
        '## Real Depth'
        '## Gizmo Overlay'
        '## Viewport Camera Persistence'
        '## Transform Modes W E R'
        '## Rotate Scale Visual Gizmos'
        '## Plane Translation Handles'
        '## Multiselect Delete Duplicate'
        '## Undo Redo Transactions'
        '## Multiselect'
        '## Active Camera Authoring'
        '## Render Debug And Depth View'
        '## Render Debug Controls'
        '## Environment Panel'
        '## Typed Scene Authoring Settings'
        '## Scene Dirty UX'
        '## Scene Authoring Migration'
        '## No New Samples Guard'
        '## Coherence Sweep'
        '## Hierarchy Authoring Workflow'
        '## Selection Inspector Transaction Workflow'
        '## Camera Gizmo Render Debug Workflow'
        '## Vertical Slice Coherence Sweep'
        '## Solid Material Authoring'
        '## Material Runtime Rendering'
        '## Sun Time Of Day'
        '## Raymarched Clouds'
        '## Exposure Tonemapping'
        '## Scene Runtime Entities'
        '## FPS Pawn Play Mode'
        '## Player Start Authoring'
        '## Play Stop Runtime Commands'
        '## Character Spine Runtime Integration'
        '## Runtime Play State Stats'
        '## FPS Pawn Tuning'
        '## Formal Play Input Capture State'
        '## Play Mode Input Capture'
        '## Collision Debug Overlay Stats'
        '## Blockout Primitive Helpers'
        '## Save Load Persistence Matrix'
        '## FPS Blockout Authoring'
        '## Hierarchy Selection Sync'
        '## Quit Close Editor'
    )
    local heading

    log "checking manual editor checklist coverage"
    for heading in "${required_headings[@]}"; do
        if ! rg -n --fixed-strings "$heading" "$MANUAL_CHECKLIST" >/dev/null; then
            log "manual checklist is missing required section: $heading"
            return 1
        fi
    done
}

require_pattern() {
    local label=$1
    local pattern=$2
    local file=$3

    if [[ ! -f "$file" ]]; then
        log "missing file for $label: $file"
        return 1
    fi
    if ! rg -n --pcre2 "$pattern" "$file" >/dev/null; then
        log "missing static anchor for $label in $file"
        return 1
    fi
}

CURRENT_WORKFLOW=''

begin_workflow_contract() {
    CURRENT_WORKFLOW=$1
    log "checking workflow contract: $CURRENT_WORKFLOW"
}

require_workflow_pattern() {
    local label=$1
    local pattern=$2
    local file=$3

    if [[ ! -f "$file" ]]; then
        log "workflow '$CURRENT_WORKFLOW' is missing file for $label: $file"
        return 1
    fi
    if ! rg -n --pcre2 "$pattern" "$file" >/dev/null; then
        log "workflow '$CURRENT_WORKFLOW' is missing anchor for $label in $file"
        return 1
    fi
}

note_optional_workflow_pattern() {
    local label=$1
    local pattern=$2
    local file=$3

    if [[ ! -f "$file" ]]; then
        log "workflow '$CURRENT_WORKFLOW' optional file is missing for $label: $file"
        return 0
    fi
    if ! rg -n --pcre2 "$pattern" "$file" >/dev/null; then
        log "workflow '$CURRENT_WORKFLOW' optional anchor not found yet: $label"
    fi
}

check_editor_workflow_contracts() {
    local scene_view="$ROOT/engine/editor/web/src/panels/SceneView.tsx"
    local app_view="$ROOT/engine/editor/web/src/App.tsx"
    local inspector="$ROOT/engine/editor/web/src/panels/Inspector.tsx"
    local crate_sample="$ROOT/samples/02_crate/main.cpp"
    local crate_scene_doc="$ROOT/samples/02_crate/SceneDocument.h"
    local scene_authoring_header="$ROOT/engine/editor/scene_authoring/SceneDocument.h"
    local scene_authoring_cpp="$ROOT/engine/editor/scene_authoring/SceneDocument.cpp"
    local panels_css="$ROOT/engine/editor/web/src/styles/panels.css"
    local protocol="$ROOT/engine/editor/web/src/ipc/protocol.ts"

    log "checking editor workflow contracts"

    begin_workflow_contract "hierarchy create rename delete duplicate"
    require_workflow_pattern \
        "single create menu actions" \
        'psy-scene-primitive-actions' \
        "$scene_view"
    require_workflow_pattern \
        "camera creation" \
        'const add_camera' \
        "$scene_view"
    require_workflow_pattern \
        "primitive creation" \
        'const add_primitive' \
        "$scene_view"
    require_workflow_pattern \
        "inline rename begin" \
        'begin_rename_entity' \
        "$scene_view"
    require_workflow_pattern \
        "inline rename commit" \
        'commit_entity_rename' \
        "$scene_view"
    require_workflow_pattern \
        "keyboard rename entry" \
        "event\\.key === 'F2'" \
        "$scene_view"
    require_workflow_pattern \
        "delete selected set" \
        'delete_selected_entities' \
        "$scene_view"
    require_workflow_pattern \
        "duplicate selected set" \
        'duplicate_selected_entities' \
        "$scene_view"
    require_workflow_pattern \
        "clear selection action" \
        'clear_scene_selection' \
        "$scene_view"
    require_workflow_pattern \
        "active camera row action" \
        'set_active_camera' \
        "$scene_view"
    require_workflow_pattern \
        "hierarchy row density styling" \
        'psy-scene-entity-row' \
        "$panels_css"
    note_optional_workflow_pattern \
        "hierarchy search/filter input" \
        '(hierarchy|entity).*(search|filter)|(search|filter).*(hierarchy|entity)' \
        "$scene_view"

    begin_workflow_contract "hierarchy multiselect and selection sync"
    require_workflow_pattern \
        "hierarchy multiselect state" \
        'hierarchy_selection' \
        "$scene_view"
    require_workflow_pattern \
        "hierarchy range multiselect" \
        'event\?\.shiftKey' \
        "$scene_view"
    require_workflow_pattern \
        "hierarchy additive multiselect" \
        'event\?\.(metaKey|ctrlKey)' \
        "$scene_view"
    require_workflow_pattern \
        "single selection publish" \
        'publish_selection' \
        "$scene_view"
    require_workflow_pattern \
        "multi selection publish" \
        'publish_hierarchy_selection' \
        "$scene_view"
    require_workflow_pattern \
        "multi selection payload" \
        'multi_selection_for_scene_entities' \
        "$scene_view"
    require_workflow_pattern \
        "cleared selection payload" \
        "type:[[:space:]]*'cleared'" \
        "$scene_view"
    require_workflow_pattern \
        "runtime selected entity patch" \
        'patch_selected_entity_json' \
        "$crate_sample"
    require_workflow_pattern \
        "runtime viewport picking" \
        'pick_scene_primitive' \
        "$crate_sample"

    begin_workflow_contract "inspector single and multiselect"
    require_workflow_pattern \
        "multiselect payload selected_entities" \
        'selected_entities' \
        "$inspector"
    require_workflow_pattern \
        "multiselect payload entity_ids" \
        'entity_ids' \
        "$inspector"
    require_workflow_pattern \
        "common component inference" \
        'common_components_from_payload' \
        "$inspector"
    require_workflow_pattern \
        "common component summary" \
        'describe_common_components' \
        "$inspector"
    require_workflow_pattern \
        "single entity commit source" \
        "source:[[:space:]]*'inspector'" \
        "$inspector"
    require_workflow_pattern \
        "single entity committed edits" \
        'commit:[[:space:]]*true' \
        "$inspector"
    require_workflow_pattern \
        "scene patch bus use" \
        'apply_scene_selection_patch' \
        "$inspector"

    begin_workflow_contract "transactions and undo redo"
    require_workflow_pattern \
        "history push" \
        'push_history' \
        "$scene_view"
    require_workflow_pattern \
        "undo action" \
        'undo_scene' \
        "$scene_view"
    require_workflow_pattern \
        "redo action" \
        'redo_scene' \
        "$scene_view"
    require_workflow_pattern \
        "undo redo keyboard path" \
        "key === 'z'" \
        "$scene_view"
    require_workflow_pattern \
        "redo keyboard path" \
        "key === 'y'" \
        "$scene_view"
    require_workflow_pattern \
        "undo redo history counts" \
        'history_counts' \
        "$scene_view"
    require_workflow_pattern \
        "undo redo toolbar" \
        'psy-scene-history-actions' \
        "$scene_view"
    require_workflow_pattern \
        "engine gizmo transaction base" \
        'engine_gizmo_history_base' \
        "$scene_view"
    require_workflow_pattern \
        "gizmo drag transaction begin" \
        'gizmo drag begin' \
        "$scene_view"
    require_workflow_pattern \
        "gizmo drag transaction commit" \
        'gizmo drag commit' \
        "$scene_view"
    require_workflow_pattern \
        "gizmo drag transaction history push" \
        'push_history\(base\)' \
        "$scene_view"

    begin_workflow_contract "camera controls and viewport persistence"
    require_workflow_pattern \
        "runtime camera update loop" \
        'update_editor_camera' \
        "$crate_sample"
    require_workflow_pattern \
        "RMB camera look" \
        'mouse\.right' \
        "$crate_sample"
    require_workflow_pattern \
        "MMB camera pan" \
        'mouse\.middle' \
        "$crate_sample"
    require_workflow_pattern \
        "wheel camera dolly" \
        'mouse\.wheel' \
        "$crate_sample"
    require_workflow_pattern \
        "F focus camera path" \
        'KeyCode::F' \
        "$crate_sample"
    require_workflow_pattern \
        "viewport camera runtime patch" \
        'sync_editor_viewport_camera' \
        "$crate_sample"
    require_workflow_pattern \
        "viewport camera typed patch" \
        'patch_viewport_camera_json' \
        "$scene_authoring_header"
    require_workflow_pattern \
        "viewport camera sanitization" \
        'sanitize_viewport_camera' \
        "$scene_authoring_cpp"

    begin_workflow_contract "gizmo modes and render depth"
    require_workflow_pattern \
        "engine XY plane translation handle" \
        'GizmoAxis::PlaneXY' \
        "$crate_sample"
    require_workflow_pattern \
        "engine XZ plane translation handle" \
        'GizmoAxis::PlaneXZ' \
        "$crate_sample"
    require_workflow_pattern \
        "engine YZ plane translation handle" \
        'GizmoAxis::PlaneYZ' \
        "$crate_sample"
    require_workflow_pattern \
        "engine plane drag basis" \
        'gizmo_plane_basis' \
        "$crate_sample"
    require_workflow_pattern \
        "rotate gizmo runtime path" \
        'TransformMode::Rotate' \
        "$crate_sample"
    require_workflow_pattern \
        "scale gizmo runtime path" \
        'TransformMode::Scale' \
        "$crate_sample"
    require_workflow_pattern \
        "rotate gizmo visuals" \
        'build_rotate_gizmo_boxes' \
        "$crate_sample"
    require_workflow_pattern \
        "scale gizmo visuals" \
        'build_scale_gizmo_boxes' \
        "$crate_sample"
    require_workflow_pattern \
        "scene depth target" \
        'scene_depth' \
        "$crate_sample"
    require_workflow_pattern \
        "scene depth allocator" \
        'ensure_scene_depth_target' \
        "$crate_sample"
    require_workflow_pattern \
        "scene depth format" \
        'Depth32Float' \
        "$crate_sample"
    require_workflow_pattern \
        "scene pass depth attachment" \
        'scene_pass\.depth\.target' \
        "$crate_sample"
    require_workflow_pattern \
        "scene pass depth clear" \
        'clear_depth[[:space:]]*=[[:space:]]*1\.0f' \
        "$crate_sample"
    require_workflow_pattern \
        "separate overlay pipeline" \
        'overlay_pipeline' \
        "$crate_sample"
    require_workflow_pattern \
        "depth debug mode" \
        'RenderDebugMode::Depth' \
        "$crate_sample"

    begin_workflow_contract "scene robustness and environment"
    require_workflow_pattern \
        "environment scene state" \
        'environmentSettings' \
        "$scene_view"
    require_workflow_pattern \
        "environment clear color" \
        'clear_color' \
        "$scene_view"
    require_workflow_pattern \
        "environment sky mode" \
        'sky_mode' \
        "$scene_view"
    require_workflow_pattern \
        "environment color edit handler" \
        'update_clear_color_hex' \
        "$scene_view"
    require_workflow_pattern \
        "invalid JSON save block" \
        'parse_scene_json_strict' \
        "$scene_view"
    require_workflow_pattern \
        "dirty state tracking" \
        'set_dirty' \
        "$scene_view"
    require_workflow_pattern \
        "scene command failures surface" \
        'scene command failed' \
        "$scene_view"
    require_workflow_pattern \
        "selected entity removal clears active camera" \
        'active_camera:[[:space:]]*cur\.active_camera === name \? null' \
        "$scene_view"
    require_workflow_pattern \
        "add camera sets active camera" \
        'active_camera:[[:space:]]*name' \
        "$scene_view"
    require_workflow_pattern \
        "typed transform mode settings" \
        'enum class TransformMode' \
        "$scene_authoring_header"
    require_workflow_pattern \
        "typed render debug settings" \
        'enum class RenderDebugMode' \
        "$scene_authoring_header"
    require_workflow_pattern \
        "typed transform mode JSON patch" \
        'patch_transform_mode_json' \
        "$scene_authoring_header"
    require_workflow_pattern \
        "typed render debug JSON patch" \
        'patch_render_debug_mode_json' \
        "$scene_authoring_header"
    require_workflow_pattern \
        "typed authoring state ensure helper" \
        'ensure_authoring_state_json' \
        "$scene_authoring_header"

    begin_workflow_contract "render debug convergence"
    require_workflow_pattern \
        "runtime render_debug command" \
        'render_debug' \
        "$crate_sample"
    require_workflow_pattern \
        "runtime depth_view command" \
        'depth_view' \
        "$crate_sample"
    require_workflow_pattern \
        "runtime render debug sync" \
        'sync_editor_render_debug' \
        "$crate_sample"
    require_workflow_pattern \
        "editor render debug setting" \
        'render_debug_from_settings' \
        "$scene_view"
    require_workflow_pattern \
        "render debug UI styling" \
        'psy-(scene-)?render-debug' \
        "$panels_css"

    begin_workflow_contract "vertical slice rendering and FPS readiness"
    require_workflow_pattern \
        "material asset category protocol" \
        "\\| 'material'" \
        "$protocol"
    require_workflow_pattern \
        "solid material protocol" \
        'interface SceneSolidMaterial' \
        "$protocol"
    require_workflow_pattern \
        "environment sun protocol" \
        'interface SceneSunSettings' \
        "$protocol"
    require_workflow_pattern \
        "environment cloud protocol" \
        'interface SceneCloudSettings' \
        "$protocol"
    require_workflow_pattern \
        "exposure protocol" \
        'exposure\?: number' \
        "$protocol"
    require_workflow_pattern \
        "entity material field protocol" \
        'material\?: SceneSolidMaterial' \
        "$protocol"
    require_workflow_pattern \
        "runtime scene view typed entry" \
        'struct RuntimeSceneView' \
        "$scene_authoring_header"
    require_workflow_pattern \
        "runtime scene view parser" \
        'parse_runtime_scene_view' \
        "$scene_authoring_header"
    require_workflow_pattern \
        "sample material component bridge" \
        'MaterialRef' \
        "$crate_sample"
    note_optional_workflow_pattern \
        "scene_authoring serializes sun settings" \
        'sun|time_of_day_hours|intensity_lux' \
        "$scene_authoring_cpp"
    note_optional_workflow_pattern \
        "scene_authoring serializes cloud settings" \
        'clouds|coverage|density|height_m|thickness_m' \
        "$scene_authoring_cpp"
    note_optional_workflow_pattern \
        "runtime material lighting path" \
        'roughness|metallic|albedo|material' \
        "$crate_sample"
    note_optional_workflow_pattern \
        "runtime cloud or sky pass" \
        'cloud|raymarch|time_of_day|sun' \
        "$crate_sample"
    note_optional_workflow_pattern \
        "FPS pawn or play mode runtime path" \
        'fps|pawn|play_mode|PlayMode|character' \
        "$crate_sample"

    begin_workflow_contract "save load persistence matrix"
    require_workflow_pattern \
        "scene save command carries JSON" \
        "action === 'save' \\? scene_text\\(doc\\) : ''" \
        "$scene_view"
    require_workflow_pattern \
        "scene load action tracked" \
        'is_scene_io_action' \
        "$scene_view"
    require_workflow_pattern \
        "scene commit keeps selected entity" \
        'selected_entity:[[:space:]]*next_selection' \
        "$scene_view"
    require_workflow_pattern \
        "scene commit keeps hierarchy selection" \
        'hierarchySelection:[[:space:]]*hierarchy_selection_ref\.current' \
        "$scene_view"
    require_workflow_pattern \
        "environment edits preserve selection" \
        'selectionName:[[:space:]]*selected_entity' \
        "$scene_view"
    require_workflow_pattern \
        "material edit handlers" \
        'update_selected_material_(rgb|number)' \
        "$scene_view"
    require_workflow_pattern \
        "edit handlers preserve hierarchy selection" \
        'hierarchySelection:[[:space:]]*hierarchy_selection_ref\.current' \
        "$scene_view"
    require_workflow_pattern \
        "player controller edit handler" \
        'update_selected_player_controller_number' \
        "$scene_view"
    require_workflow_pattern \
        "scene_authoring serializes material" \
        'material_json|\"material\"' \
        "$scene_authoring_cpp"
    require_workflow_pattern \
        "scene_authoring serializes PlayerController" \
        'player_controller_json|\"player_controller\"' \
        "$scene_authoring_cpp"
    require_workflow_pattern \
        "scene_authoring serializes environment" \
        '\"environmentSettings\"' \
        "$scene_authoring_cpp"
    require_workflow_pattern \
        "scene_authoring serializes selected entity" \
        '\"selected_entity\"' \
        "$scene_authoring_cpp"
    require_workflow_pattern \
        "runtime parser bridges PlayerController" \
        'ScenePlayerController|player_controllers' \
        "$crate_scene_doc"

    begin_workflow_contract "play mode vertical slice anchors"
    require_workflow_pattern \
        "scene_authoring player start contract" \
        'has_player_start|PlayerStartView|player_starts' \
        "$scene_authoring_header"
    require_workflow_pattern \
        "sample parser player start bridge" \
        'PlayerStart|player_start|player_starts' \
        "$crate_scene_doc"
    require_workflow_pattern \
        "runtime play command" \
        '"play"' \
        "$crate_sample"
    require_workflow_pattern \
        "runtime stop command" \
        '"stop"' \
        "$crate_sample"
    require_workflow_pattern \
        "runtime play mode state" \
        'PlayMode|play_mode|play_state' \
        "$crate_sample"
    require_workflow_pattern \
        "runtime character spine integration" \
        'character_spine|CharacterSpine' \
        "$crate_sample"
    require_workflow_pattern \
        "runtime stats report play state" \
        'play_state:|play_mode:' \
        "$crate_sample"

    begin_workflow_contract "next play mode tuning anchors"
    require_workflow_pattern \
        "FPS pawn tuning fields" \
        'PlayerController|walk_speed|run_speed|jump_velocity|mouse_sensitivity|capsule_radius|capsule_height' \
        "$scene_authoring_header"
    require_workflow_pattern \
        "FPS pawn tuning UI" \
        'Player Controller|walk speed|run speed|jump|mouse sensitivity|capsule' \
        "$scene_view"
    require_workflow_pattern \
        "runtime pawn tuning application" \
        'play_tuning_from_controller|walk_speed|run_speed|jump_velocity|mouse_sensitivity' \
        "$crate_sample"

    begin_workflow_contract "play mode input capture anchors"
    require_workflow_pattern \
        "play mode mouse capture state" \
        'input_captured|input_capture|play_capture_input' \
        "$crate_sample"
    require_workflow_pattern \
        "play mode input capture command" \
        '"input_capture"' \
        "$crate_sample"
    require_workflow_pattern \
        "play mode mouse look without RMB" \
        'play_mode.*mouse|mouse.*play_mode|nav_mouse|mouse_sensitivity|look_sensitivity' \
        "$crate_sample"
    require_workflow_pattern \
        "editor console blocks play input capture" \
        'text_input|console_text|input_captured[[:space:]]*=[[:space:]]*false' \
        "$crate_sample"

    begin_workflow_contract "collision debug overlay stats anchors"
    require_workflow_pattern \
        "collision debug command or cvar" \
        '"collision_debug"|collision_debug' \
        "$crate_sample"
    require_workflow_pattern \
        "collision overlay render path" \
        'play\.collision_debug|play_mode\.collision_debug|collision_debug' \
        "$crate_sample"
    require_workflow_pattern \
        "collision stats counters" \
        'static_collider_count|play_colliders|static_colliders' \
        "$crate_sample"

    begin_workflow_contract "FPS blockout helper and hierarchy sync anchors"
    require_workflow_pattern \
        "blockout primitive helpers" \
        'BLOCKOUT_PRESETS|add_blockout_preset' \
        "$scene_view"
    require_workflow_pattern \
        "blockout floor preset" \
        "'floor'" \
        "$scene_view"
    require_workflow_pattern \
        "blockout wall preset" \
        "'wall'" \
        "$scene_view"
    require_workflow_pattern \
        "blockout ramp preset" \
        "'ramp'" \
        "$scene_view"
    require_workflow_pattern \
        "blockout cover preset" \
        "'cover'" \
        "$scene_view"
    require_workflow_pattern \
        "blockout helper styling" \
        'psy-blockout-helper' \
        "$panels_css"
    require_workflow_pattern \
        "hierarchy selection sync publish" \
        'publish_hierarchy_selection|publish_selection' \
        "$scene_view"
    require_workflow_pattern \
        "selection patch bus" \
        'apply_scene_selection_patch' \
        "$inspector"
    note_optional_workflow_pattern \
        "runtime selection stats" \
        'selected_entity|selected_entities|selection_stats|hierarchy_selection' \
        "$crate_sample"

    begin_workflow_contract "quit close editor anchors"
    require_workflow_pattern \
        "scene panel close engine action" \
        'close_engine|send_console_command\('"'"'quit'"'"'\)' \
        "$scene_view"
    require_workflow_pattern \
        "app quit engine and editor action" \
        'engine_power_action|Quit engine and editor|source:[[:space:]]*'"'"'quit'"'"'' \
        "$app_view"
    require_workflow_pattern \
        "runtime quit request consumption" \
        'consume_runtime_console_quit_requested|request_quit' \
        "$crate_sample"
}

check_no_new_samples_guard() {
    local sample_dir="$ROOT/samples"
    local allowed=' 00_clear 01_triangle 02_crate '
    local dir
    local name

    log "checking no new sample directories were added"
    while IFS= read -r -d '' dir; do
        name=$(basename "$dir")
        if [[ "$allowed" != *" $name "* ]]; then
            log "unexpected sample directory: $dir"
            return 1
        fi
    done < <(find "$sample_dir" -mindepth 1 -maxdepth 1 -type d -print0)
}

run_runtime_smoke() {
    local smoke_script="$ROOT/scripts/smoke_sample.sh"
    if [[ ! -x "$smoke_script" ]]; then
        log "missing executable smoke helper: $smoke_script"
        return 1
    fi
    run "$smoke_script" "$PSY_ARCADE_BIN" "--smoke-frames=$SMOKE_FRAMES" "--quiet"
}

cd "$ROOT"

log "root: $ROOT"
log "preset: $PRESET"

if [[ "$SKIP_WEB_BUILD" != "1" ]]; then
    run npm --prefix "$WEB_DIR" run build
else
    log "skipping web build"
fi

if [[ "$SKIP_BUILDS" != "1" ]]; then
    run cmake --build --preset "$PRESET" --target PsyArcadeGX
    run cmake --build --preset "$PRESET" --target sample_02_crate
else
    log "skipping CMake builds"
fi

if [[ "$SKIP_GREP_CHECKS" != "1" ]]; then
    check_fake_web_gizmo_absent
    check_manual_checklist_coverage
    check_editor_workflow_contracts
    check_no_new_samples_guard
else
    log "skipping grep checks"
fi

if [[ "$SKIP_RUNTIME_SMOKE" != "1" ]]; then
    run_runtime_smoke
else
    log "skipping runtime smoke"
fi

log "editor loop smoke passed"

if [[ "$PRINT_MANUAL_CHECKLIST" == "1" ]]; then
    printf '\n'
    log "manual interaction checklist: $MANUAL_CHECKLIST"
    sed -n '1,220p' "$MANUAL_CHECKLIST"
fi
