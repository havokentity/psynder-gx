// SPDX-License-Identifier: MIT
// Psynder-GX editor — first scene authoring surface. This is the loose-file
// loop that lets the player boot from editor-created scenes while the cooker
// and packed scene format are still coming online.

import React from 'react';

import { get_client } from '../ipc/client';
import type {
    ComponentSchema,
    ConsoleEval,
    Envelope,
    SelectionState,
    SceneCommandRequest,
    SceneCommandResult,
    SceneDocument,
    SceneRgb,
    SceneSolidMaterial,
    SceneStatus,
} from '../ipc/protocol';
import { PROTOCOL_VERSION } from '../ipc/protocol';
import { ConnectionBadge } from './shared/ConnectionBadge';
import {
    multi_selection_for_scene_entities,
    resolve_scene_patch_entity_index,
    set_scene_patch_handler,
    type SceneSelectionPatch,
} from './shared/sceneAuthoringBus';

const DEFAULT_PATH = 'projects/PsyArcadeGX/scenes/startup.scene.bin';
const DEFAULT_NAME = 'Startup';
const PRIMITIVES = ['cube', 'sphere', 'plane'] as const;
const BLOCKOUT_PRESETS = ['floor', 'wall', 'block', 'ramp', 'cover'] as const;
const TRANSFORM_MODES = ['translate', 'rotate', 'scale'] as const;
const RENDER_DEBUG_MODES = ['off', 'depth'] as const;
type PrimitiveKind = typeof PRIMITIVES[number];
type BlockoutPreset = typeof BLOCKOUT_PRESETS[number];
type TransformMode = typeof TRANSFORM_MODES[number];
type RenderDebugMode = typeof RENDER_DEBUG_MODES[number];
type SceneEntity = SceneDocument['entities'][number];
type PlayerControllerSettings = {
    capsule_radius_m: number;
    capsule_height_m: number;
    eye_height_m: number;
    walk_speed_mps: number;
    run_speed_mps: number;
    jump_speed_mps: number;
    mouse_sensitivity: number;
};
type PlayerControllerKey = keyof PlayerControllerSettings;
type PlayerControllerEntity = SceneEntity & {
    player_controller?: Partial<PlayerControllerSettings>;
};
type SceneEditorSettings = NonNullable<SceneDocument['editorSettings']>;
type NormalizedEditorSettings = {
    grid_visible: boolean;
    snap_mode: 'off' | 'grid';
    snap_step: number;
};
type ScenePanelMode = 'hierarchy' | 'settings';
type SceneIoAction = Extract<SceneCommandRequest['action'], 'create' | 'load' | 'save'>;
type Vec3 = [number, number, number];
type MaterialRgbKey = 'albedo' | 'emissive';
type SceneHistory = {
    past: SceneDocument[];
    future: SceneDocument[];
};
type SceneHistoryCounts = {
    past: number;
    future: number;
};
type SceneCommitOptions = {
    label: string;
    selectionName?: string | null;
    hierarchySelection?: string[];
};
type CommitNumberInputProps = {
    value: number;
    decimals: number;
    onCommit: (value: string) => void;
    min?: number;
    max?: number;
    step?: number;
    ariaLabel?: string;
};

const HISTORY_LIMIT = 80;
const DEFAULT_CLOUD_SPEED = 0.035;
const DEFAULT_SOLID_MATERIAL: Required<SceneSolidMaterial> = {
    albedo: [0.72, 0.76, 0.82],
    roughness: 0.55,
    metallic: 0.0,
    emissive: [0.0, 0.0, 0.0],
    emissive_intensity: 0.0,
};
const DEFAULT_PLAYER_CONTROLLER: PlayerControllerSettings = {
    capsule_radius_m: 0.35,
    capsule_height_m: 1.8,
    eye_height_m: 1.62,
    walk_speed_mps: 4.6,
    run_speed_mps: 7.0,
    jump_speed_mps: 5.2,
    mouse_sensitivity: 0.10,
};

function CommitNumberInput({
    value,
    decimals,
    onCommit,
    min,
    max,
    step,
    ariaLabel,
}: CommitNumberInputProps) {
    const formatted = React.useMemo(() => value.toFixed(decimals), [decimals, value]);
    const [draft, set_draft] = React.useState(formatted);

    React.useEffect(() => {
        set_draft(formatted);
    }, [formatted]);

    const commit = React.useCallback(() => {
        if (draft === formatted) return;
        const parsed = Number(draft);
        if (!Number.isFinite(parsed)) {
            set_draft(formatted);
            return;
        }
        onCommit(draft);
    }, [draft, formatted, onCommit]);

    return (
        <input
            aria-label={ariaLabel}
            className="psy-input"
            type="text"
            inputMode="decimal"
            min={min}
            max={max}
            step={step}
            value={draft}
            onChange={(e) => set_draft(e.target.value)}
            onBlur={commit}
            onKeyDown={(e) => {
                if (e.key === 'Enter') {
                    e.currentTarget.blur();
                } else if (e.key === 'Escape') {
                    set_draft(formatted);
                    e.currentTarget.blur();
                }
            }}
        />
    );
}

const SCENE_SCHEMAS: ComponentSchema[] = [
    {
        name: 'Transform',
        layout_hash: 'scene-loose-transform-v1',
        fields: [
            { name: 'position', kind: 'vec3', numeric: { step: 0.01, unit: 'm' } },
            { name: 'rotation_euler_deg', kind: 'vec3', numeric: { step: 0.1, unit: 'deg' } },
            { name: 'scale', kind: 'vec3', numeric: { step: 0.01 } },
        ],
    },
    {
        name: 'Camera',
        layout_hash: 'scene-loose-camera-v1',
        fields: [
            { name: 'fov_y_deg', kind: 'f32', numeric: { min: 1, max: 179, step: 0.1, unit: 'deg' } },
        ],
    },
    {
        name: 'PrimitiveMesh',
        layout_hash: 'scene-loose-primitive-v1',
        fields: [
            { name: 'primitive', kind: 'string', readonly: true },
        ],
    },
    {
        name: 'Renderable',
        layout_hash: 'scene-loose-renderable-v1',
        fields: [
            { name: 'visible', kind: 'bool' },
        ],
    },
    {
        name: 'PlayerStart',
        layout_hash: 'scene-loose-player-start-v1',
        fields: [],
    },
    {
        name: 'PlayerController',
        layout_hash: 'scene-loose-player-controller-v1',
        fields: [
            { name: 'capsule_radius_m', kind: 'f32', numeric: { min: 0.05, max: 2.0, step: 0.01, unit: 'm' } },
            { name: 'capsule_height_m', kind: 'f32', numeric: { min: 0.2, max: 4.0, step: 0.01, unit: 'm' } },
            { name: 'eye_height_m', kind: 'f32', numeric: { min: 0.1, max: 4.0, step: 0.01, unit: 'm' } },
            { name: 'walk_speed_mps', kind: 'f32', numeric: { min: 0.0, max: 20.0, step: 0.1, unit: 'm/s' } },
            { name: 'run_speed_mps', kind: 'f32', numeric: { min: 0.0, max: 30.0, step: 0.1, unit: 'm/s' } },
            { name: 'jump_speed_mps', kind: 'f32', numeric: { min: 0.0, max: 20.0, step: 0.1, unit: 'm/s' } },
            { name: 'mouse_sensitivity', kind: 'f32', numeric: { min: 0.01, max: 2.0, step: 0.01 } },
        ],
    },
];

function default_scene(): SceneDocument {
    return {
        format: 'psynder-gx-loose-scene-v1',
        name: DEFAULT_NAME,
        path: DEFAULT_PATH,
        active_camera: 'Camera',
        selected_entity: null,
        editorSettings: {
            grid_visible: true,
            transform_mode: 'translate',
        },
        environmentSettings: {
            clear_color: [0.018, 0.027, 0.050, 1.0],
            sky_mode: 'sdf-boot-field',
        },
        entities: [{
            name: 'Camera',
            components: ['Transform', 'Camera'],
            position: [0.0, 1.4, 4.2],
            rotation_euler_deg: [-8.0, 180.0, 0.0],
            fov_y_deg: 70.0,
        }],
    };
}

function parse_scene_json(text: string): SceneDocument {
    try {
        return normalize_scene_json(JSON.parse(text) as Partial<SceneDocument>);
    } catch {
        return default_scene();
    }
}

function parse_scene_json_strict(text: string): SceneDocument | null {
    try {
        return normalize_scene_json(JSON.parse(text) as Partial<SceneDocument>);
    } catch {
        return null;
    }
}

function normalize_scene_json(parsed: Partial<SceneDocument>): SceneDocument {
    return {
        ...default_scene(),
        ...parsed,
        environmentSettings: {
            ...default_scene().environmentSettings,
            ...(parsed.environmentSettings ?? {}),
        },
        editorSettings: {
            ...default_scene().editorSettings,
            ...(parsed.editorSettings ?? {}),
        },
        entities: Array.isArray(parsed.entities) ? parsed.entities as SceneDocument['entities'] : [],
    };
}

function scene_text(scene: SceneDocument): string {
    return JSON.stringify(scene, null, 2) + '\n';
}

function clone_scene(scene: SceneDocument): SceneDocument {
    return parse_scene_json(scene_text(scene));
}

function same_scene(a: SceneDocument, b: SceneDocument): boolean {
    return scene_text(a) === scene_text(b);
}

function same_string_list(a: readonly string[], b: readonly string[]): boolean {
    return a.length === b.length && a.every((value, index) => value === b[index]);
}

function clone_entity(entity: SceneEntity): SceneEntity {
    return clone_scene({
        ...default_scene(),
        entities: [entity],
    }).entities[0];
}

function unique_entity_name(entities: SceneEntity[], base: string): string {
    const used = new Set(entities.map((e) => e.name));
    if (!used.has(base)) return base;
    for (let i = 2; i < 10_000; ++i) {
        const next = `${base}_${i}`;
        if (!used.has(next)) return next;
    }
    return `${base}_${Date.now()}`;
}

function entity_name_set(scene: SceneDocument): Set<string> {
    return new Set(scene.entities.map((entity) => entity.name));
}

function sanitize_entity_names(scene: SceneDocument, names: readonly string[]): string[] {
    const existing = entity_name_set(scene);
    const seen = new Set<string>();
    const sanitized: string[] = [];
    for (const name of names) {
        if (!existing.has(name) || seen.has(name)) {
            continue;
        }
        seen.add(name);
        sanitized.push(name);
    }
    return sanitized;
}

function default_selection_for_scene(
    scene: SceneDocument,
    preferred: readonly string[] = [],
): string[] {
    const sanitized = sanitize_entity_names(scene, preferred);
    if (sanitized.length > 0) {
        const selected = scene.selected_entity ?? sanitized[0];
        return sanitized.includes(selected)
            ? sanitized
            : sanitize_entity_names(scene, [selected, ...sanitized]);
    }
    return scene.selected_entity
        ? sanitize_entity_names(scene, [scene.selected_entity])
        : [];
}

function clamp01(value: number): number {
    return Math.min(1, Math.max(0, Number.isFinite(value) ? value : 0));
}

function clamp_range(value: number, min: number, max: number, fallback: number): number {
    const parsed = Number.isFinite(value) ? value : fallback;
    return Math.min(max, Math.max(min, parsed));
}

function finite_number(value: unknown, fallback = 0): number {
    return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}

function editor_settings(scene: SceneDocument): NormalizedEditorSettings {
    const raw = scene.editorSettings ?? {};
    const snap_mode = raw.snap_mode === 'grid' ? 'grid' : 'off';
    return {
        grid_visible: raw.grid_visible ?? true,
        snap_mode,
        snap_step: finite_number(raw.snap_step, 1.0),
    };
}

function transform_mode_from_settings(scene: SceneDocument): TransformMode {
    const mode = scene.editorSettings?.transform_mode;
    return typeof mode === 'string' &&
        (TRANSFORM_MODES as readonly string[]).includes(mode)
        ? mode as TransformMode
        : 'translate';
}

function render_debug_from_settings(scene: SceneDocument): RenderDebugMode {
    const mode = scene.editorSettings?.render_debug;
    return typeof mode === 'string' &&
        (RENDER_DEBUG_MODES as readonly string[]).includes(mode)
        ? mode as RenderDebugMode
        : 'off';
}

function vec3_value(value: SceneEntity['position'], fallback: Vec3): Vec3 {
    return [
        finite_number(value?.[0], fallback[0]),
        finite_number(value?.[1], fallback[1]),
        finite_number(value?.[2], fallback[2]),
    ];
}

function display_num(value: number): string {
    const fixed = Math.abs(value) >= 100 ? value.toFixed(1) : value.toFixed(3);
    return fixed.replace(/\.?0+$/, '');
}

function clear_color(scene: SceneDocument): [number, number, number, number] {
    const c = scene.environmentSettings.clear_color;
    return [clamp01(c[0]), clamp01(c[1]), clamp01(c[2]), clamp01(c[3])];
}

function clear_color_hex(scene: SceneDocument): string {
    const [r, g, b] = clear_color(scene);
    const byte = (v: number) => Math.round(clamp01(v) * 255).toString(16).padStart(2, '0');
    return `#${byte(r)}${byte(g)}${byte(b)}`;
}

function rgb01(value: unknown, fallback: SceneRgb): SceneRgb {
    if (!Array.isArray(value) || value.length < 3) return fallback;
    return [
        clamp01(finite_number(value[0], fallback[0])),
        clamp01(finite_number(value[1], fallback[1])),
        clamp01(finite_number(value[2], fallback[2])),
    ];
}

function rgb_hex(rgb: SceneRgb): string {
    const byte = (v: number) => Math.round(clamp01(v) * 255).toString(16).padStart(2, '0');
    return `#${byte(rgb[0])}${byte(rgb[1])}${byte(rgb[2])}`;
}

function hex_to_rgb01(hex: string): [number, number, number] | null {
    const m = hex.match(/^#?([0-9a-f]{6})$/i);
    if (!m) return null;
    const raw = m[1];
    return [
        parseInt(raw.slice(0, 2), 16) / 255,
        parseInt(raw.slice(2, 4), 16) / 255,
        parseInt(raw.slice(4, 6), 16) / 255,
    ];
}

function solid_material(entity: SceneEntity | null): Required<SceneSolidMaterial> {
    const material = entity?.material ?? {};
    return {
        albedo: rgb01(material.albedo, DEFAULT_SOLID_MATERIAL.albedo),
        roughness: clamp01(finite_number(material.roughness, DEFAULT_SOLID_MATERIAL.roughness)),
        metallic: clamp01(finite_number(material.metallic, DEFAULT_SOLID_MATERIAL.metallic)),
        emissive: rgb01(material.emissive, DEFAULT_SOLID_MATERIAL.emissive),
        emissive_intensity: clamp_range(
            finite_number(material.emissive_intensity, DEFAULT_SOLID_MATERIAL.emissive_intensity),
            0.0,
            64.0,
            DEFAULT_SOLID_MATERIAL.emissive_intensity,
        ),
    };
}

function player_controller(entity: SceneEntity | null): PlayerControllerSettings {
    const raw = (entity as PlayerControllerEntity | null)?.player_controller ?? {};
    return {
        capsule_radius_m: clamp_range(
            finite_number(raw.capsule_radius_m, DEFAULT_PLAYER_CONTROLLER.capsule_radius_m),
            0.05,
            2.0,
            DEFAULT_PLAYER_CONTROLLER.capsule_radius_m,
        ),
        capsule_height_m: clamp_range(
            finite_number(raw.capsule_height_m, DEFAULT_PLAYER_CONTROLLER.capsule_height_m),
            0.2,
            4.0,
            DEFAULT_PLAYER_CONTROLLER.capsule_height_m,
        ),
        eye_height_m: clamp_range(
            finite_number(raw.eye_height_m, DEFAULT_PLAYER_CONTROLLER.eye_height_m),
            0.1,
            4.0,
            DEFAULT_PLAYER_CONTROLLER.eye_height_m,
        ),
        walk_speed_mps: clamp_range(
            finite_number(raw.walk_speed_mps, DEFAULT_PLAYER_CONTROLLER.walk_speed_mps),
            0.0,
            20.0,
            DEFAULT_PLAYER_CONTROLLER.walk_speed_mps,
        ),
        run_speed_mps: clamp_range(
            finite_number(raw.run_speed_mps, DEFAULT_PLAYER_CONTROLLER.run_speed_mps),
            0.0,
            30.0,
            DEFAULT_PLAYER_CONTROLLER.run_speed_mps,
        ),
        jump_speed_mps: clamp_range(
            finite_number(raw.jump_speed_mps, DEFAULT_PLAYER_CONTROLLER.jump_speed_mps),
            0.0,
            20.0,
            DEFAULT_PLAYER_CONTROLLER.jump_speed_mps,
        ),
        mouse_sensitivity: clamp_range(
            finite_number(raw.mouse_sensitivity, DEFAULT_PLAYER_CONTROLLER.mouse_sensitivity),
            0.01,
            2.0,
            DEFAULT_PLAYER_CONTROLLER.mouse_sensitivity,
        ),
    };
}

function status_from_payload(payload: unknown): SceneStatus {
    const p = payload as Partial<SceneStatus> | undefined;
    return {
        name: typeof p?.name === 'string' ? p.name : DEFAULT_NAME,
        path: typeof p?.path === 'string' ? p.path : DEFAULT_PATH,
        has_scene: Boolean(p?.has_scene),
        has_camera: Boolean(p?.has_camera),
        dirty: Boolean(p?.dirty),
        entity_count: typeof p?.entity_count === 'number' ? p.entity_count : 0,
        message: typeof p?.message === 'string' ? p.message : '',
        document_json: typeof p?.document_json === 'string' ? p.document_json : '',
    };
}

function is_scene_io_action(action: SceneCommandRequest['action'] | undefined): action is SceneIoAction {
    return action === 'create' || action === 'load' || action === 'save';
}

function action_progress_label(action: SceneIoAction): string {
    if (action === 'create') return 'creating scene...';
    if (action === 'load') return 'loading scene...';
    return 'saving scene...';
}

function selection_for_entity(entity: SceneEntity, index: number): SelectionState {
    const components: SelectionState['components'] = {};
    if (entity.components.includes('Transform')) {
        components.Transform = {
            position: entity.position ?? [0.0, 0.0, 0.0],
            rotation_euler_deg: entity.rotation_euler_deg ?? [0.0, 0.0, 0.0],
            scale: entity.scale ?? [1.0, 1.0, 1.0],
        };
    }
    if (entity.components.includes('Camera')) {
        components.Camera = {
            fov_y_deg: entity.fov_y_deg ?? 70.0,
        };
    }
    if (entity.components.includes('PrimitiveMesh')) {
        components.PrimitiveMesh = {
            primitive: entity.primitive ?? '',
        };
    }
    if (entity.components.includes('Renderable')) {
        components.Renderable = {
            visible: entity.visible ?? true,
        };
    }
    if (entity.components.includes('PlayerStart')) {
        components.PlayerStart = {};
    }
    if (entity.components.includes('PlayerController')) {
        components.PlayerController = player_controller(entity);
    }
    return {
        entity_id: index + 1,
        entity_label: entity.name,
        components,
    };
}

function entity_kind_label(entity: SceneEntity): string {
    if (entity.components.includes('PlayerController')) return 'fps pawn';
    if (entity.components.includes('PlayerStart')) return 'player start';
    if (entity.components.includes('Camera')) return 'camera';
    return entity.primitive ?? 'entity';
}

function entity_detail_label(entity: SceneEntity): string {
    if (entity.components.includes('PlayerController')) return 'spawn + fps controller';
    if (entity.components.includes('PlayerStart')) return 'spawn point';
    if (entity.components.includes('Camera')) return 'scene camera';
    if (entity.components.includes('PrimitiveMesh')) {
        const primitive = entity.primitive ?? 'mesh';
        const scale = vec3_value(entity.scale, [1.0, 1.0, 1.0]).map(display_num).join(' x ');
        return `${primitive} / ${scale} m`;
    }
    return entity.components.join(' + ') || 'entity';
}

function entity_kind_icon(entity: SceneEntity): string {
    if (entity.components.includes('PlayerController')) return 'F';
    if (entity.components.includes('PlayerStart')) return 'P';
    if (entity.components.includes('Camera')) return 'C';
    return entity.primitive?.charAt(0).toUpperCase() ?? 'E';
}

function vec3_from_unknown(value: unknown, fallback: Vec3): Vec3 | null {
    if (!Array.isArray(value) || value.length < 3) return null;
    return [
        finite_number(value[0], fallback[0]),
        finite_number(value[1], fallback[1]),
        finite_number(value[2], fallback[2]),
    ];
}

function object_from_unknown(value: unknown): Record<string, unknown> | null {
    return value && typeof value === 'object' && !Array.isArray(value)
        ? value as Record<string, unknown>
        : null;
}

function viewport_camera_summary(scene: SceneDocument): string {
    const camera = object_from_unknown(scene.editorSettings?.viewport_camera);
    if (!camera) return 'not saved';
    const position = vec3_from_unknown(camera.position, [0.0, 0.0, 0.0]);
    const target = vec3_from_unknown(camera.target, [0.0, 0.0, 0.0]);
    const distance = finite_number(camera.distance, Number.NaN);
    if (position) {
        return `pos ${position.map(display_num).join(', ')}`;
    }
    if (target && Number.isFinite(distance)) {
        return `target ${target.map(display_num).join(', ')} / ${display_num(distance)}m`;
    }
    if (target) {
        return `target ${target.map(display_num).join(', ')}`;
    }
    return 'saved';
}

function player_start_transform(scene: SceneDocument): {
    position: Vec3;
    rotation_euler_deg: Vec3;
} {
    const camera = scene.entities.find((entity) => (
        entity.name === scene.active_camera &&
        entity.components.includes('Camera')
    )) ?? scene.entities.find((entity) => entity.components.includes('Camera'));
    if (!camera) {
        return {
            position: [0.0, 0.0, 0.0],
            rotation_euler_deg: [0.0, 0.0, 0.0],
        };
    }
    const camera_position = vec3_value(camera.position, [0.0, 1.4, 4.2]);
    const camera_rotation = vec3_value(camera.rotation_euler_deg, [0.0, 180.0, 0.0]);
    const yaw_rad = camera_rotation[1] * Math.PI / 180.0;
    return {
        position: [
            camera_position[0] + Math.sin(yaw_rad) * 2.0,
            0.0,
            camera_position[2] + Math.cos(yaw_rad) * 2.0,
        ],
        rotation_euler_deg: [0.0, camera_rotation[1], 0.0],
    };
}

function offset_duplicate_entity(entity: SceneEntity, ordinal: number): SceneEntity {
    if (!entity.components.includes('Transform')) return entity;
    const position = vec3_value(entity.position, [0.0, 0.0, 0.0]);
    const offset = Math.max(1, ordinal) * 0.75;
    return {
        ...entity,
        position: [
            position[0] + offset,
            position[1],
            position[2] + offset,
        ],
    };
}

function apply_selection_patch_to_entity(
    entity: SceneEntity,
    patch: SceneSelectionPatch,
): SceneEntity {
    if (patch.component === 'Transform') {
        if (patch.field === 'position') {
            const next = vec3_from_unknown(
                patch.value,
                vec3_value(entity.position, [0.0, 0.0, 0.0]),
            );
            return next ? { ...entity, position: next } : entity;
        }
        if (patch.field === 'rotation_euler_deg') {
            const next = vec3_from_unknown(
                patch.value,
                vec3_value(entity.rotation_euler_deg, [0.0, 0.0, 0.0]),
            );
            return next ? { ...entity, rotation_euler_deg: next } : entity;
        }
        if (patch.field === 'scale') {
            const next = vec3_from_unknown(
                patch.value,
                vec3_value(entity.scale, [1.0, 1.0, 1.0]),
            );
            return next ? { ...entity, scale: next } : entity;
        }
    }
    if (patch.component === 'Camera' && patch.field === 'fov_y_deg') {
        const fov = finite_number(patch.value, entity.fov_y_deg ?? 70.0);
        return { ...entity, fov_y_deg: Math.min(179.0, Math.max(1.0, fov)) };
    }
    if (patch.component === 'Renderable' && patch.field === 'visible') {
        return { ...entity, visible: Boolean(patch.value) };
    }
    if (patch.component === 'PlayerController') {
        const key = patch.field as PlayerControllerKey;
        if (!Object.prototype.hasOwnProperty.call(DEFAULT_PLAYER_CONTROLLER, key)) {
            return entity;
        }
        const merged = {
            ...player_controller(entity),
            [key]: finite_number(
                patch.value,
                DEFAULT_PLAYER_CONTROLLER[key],
            ),
        };
        return {
            ...entity,
            components: entity.components.includes('PlayerController')
                ? entity.components
                : [...entity.components, 'PlayerController'],
            player_controller: player_controller({
                ...entity,
                player_controller: merged,
            } as PlayerControllerEntity),
        } as SceneEntity;
    }
    return entity;
}

export function SceneView({
    title = 'Hierarchy',
    mode = 'hierarchy',
}: {
    title?: string;
    mode?: ScenePanelMode;
}) {
    const client = React.useMemo(() => get_client(), []);
    const seq = React.useRef(1);
    const [scene, set_scene] = React.useState<SceneDocument>(default_scene);
    const scene_ref = React.useRef<SceneDocument>(default_scene());
    const history_ref = React.useRef<SceneHistory>({ past: [], future: [] });
    const engine_gizmo_history_base = React.useRef<SceneDocument | null>(null);
    const pending_scene_actions = React.useRef<Map<number, SceneCommandRequest['action']>>(new Map());
    const [history_counts, set_history_counts] = React.useState<SceneHistoryCounts>({
        past: 0,
        future: 0,
    });
    const [path, set_path] = React.useState(DEFAULT_PATH);
    const [status, set_status] = React.useState<SceneStatus>(() => ({
        name: DEFAULT_NAME,
        path: DEFAULT_PATH,
        has_scene: false,
        has_camera: false,
        dirty: false,
        entity_count: 0,
        message: 'no scene open',
        document_json: '',
    }));
    const [active_scene_action, set_active_scene_action] = React.useState<SceneIoAction | null>(null);
    const [editor_text, set_editor_text] = React.useState(() => scene_text(default_scene()));
    const [json_error, set_json_error] = React.useState<string | null>(null);
    const [scene_error, set_scene_error] = React.useState<string | null>(null);
    const [dirty, set_dirty] = React.useState(false);
    const [selected_entity, set_selected_entity] = React.useState<string | null>(null);
    const [hierarchy_selection, set_hierarchy_selection] = React.useState<string[]>([]);
    const hierarchy_selection_ref = React.useRef<string[]>([]);
    const [renaming_entity, set_renaming_entity] = React.useState<string | null>(null);
    const [rename_draft, set_rename_draft] = React.useState('');
    const [hierarchy_filter, set_hierarchy_filter] = React.useState('');
    const rename_input_ref = React.useRef<HTMLInputElement | null>(null);
    const add_menu_ref = React.useRef<HTMLDetailsElement | null>(null);
    const hierarchy_selection_anchor = React.useRef<string | null>(null);
    const last_transaction_label = React.useRef<string>('bootstrap');
    const selected_index = selected_entity
        ? scene.entities.findIndex((entity) => entity.name === selected_entity)
        : -1;
    const selected = selected_index >= 0 ? scene.entities[selected_index] : null;
    const settings = editor_settings(scene);
    const grid_visible = settings.grid_visible;
    const transform_mode = transform_mode_from_settings(scene);
    const render_debug = render_debug_from_settings(scene);
    const has_render_debug_setting = Object.prototype.hasOwnProperty.call(
        scene.editorSettings ?? {},
        'render_debug',
    );
    const viewport_camera = viewport_camera_summary(scene);
    const can_undo = history_counts.past > 0;
    const can_redo = history_counts.future > 0;
    const busy = active_scene_action !== null;
    const has_dirty_scene = dirty || status.dirty;

    React.useEffect(() => {
        scene_ref.current = scene;
    }, [scene]);

    React.useEffect(() => {
        hierarchy_selection_ref.current = hierarchy_selection;
    }, [hierarchy_selection]);

    React.useEffect(() => {
        const names = new Set(scene.entities.map((entity) => entity.name));
        set_hierarchy_selection((current) => current.filter((name) => names.has(name)));
        if (hierarchy_selection_anchor.current && !names.has(hierarchy_selection_anchor.current)) {
            hierarchy_selection_anchor.current = null;
        }
        if (renaming_entity && !names.has(renaming_entity)) {
            set_renaming_entity(null);
            set_rename_draft('');
        }
    }, [renaming_entity, scene.entities]);

    React.useEffect(() => {
        if (!renaming_entity) return;
        requestAnimationFrame(() => {
            rename_input_ref.current?.focus();
            rename_input_ref.current?.select();
        });
    }, [renaming_entity]);

    const publish_history_counts = React.useCallback(() => {
        const history = history_ref.current;
        set_history_counts({
            past: history.past.length,
            future: history.future.length,
        });
    }, []);

    const push_history = React.useCallback((before: SceneDocument) => {
        const snapshot = clone_scene(before);
        const history = history_ref.current;
        const last = history.past[history.past.length - 1];
        if (last && same_scene(last, snapshot)) {
            return;
        }
        history.past = [...history.past, snapshot].slice(-HISTORY_LIMIT);
        history.future = [];
        publish_history_counts();
    }, [publish_history_counts]);

    const send_scene = React.useCallback((action: SceneCommandRequest['action']) => {
        const doc = action === 'save'
            ? parse_scene_json_strict(editor_text)
            : parse_scene_json(editor_text);
        if (!doc) {
            set_json_error('Scene JSON is invalid. Fix it before saving the scene.');
            set_scene_error(null);
            set_status((cur) => ({
                ...cur,
                message: 'blocked: invalid scene JSON',
            }));
            return;
        }
        const id = seq.current++;
        pending_scene_actions.current.set(id, action);
        if (is_scene_io_action(action)) {
            set_active_scene_action(action);
        }
        set_scene_error(null);
        doc.name = scene.name;
        doc.path = path;
        client.send<SceneCommandRequest>('scene', 'command', {
            id,
            action,
            path,
            name: scene.name,
            document_json: action === 'save' ? scene_text(doc) : '',
        });
    }, [client, editor_text, path, scene.name]);

    const send_console_command = React.useCallback((source: string) => {
        client.send<ConsoleEval>('console', 'eval', {
            id: seq.current++,
            source,
            mode: 'console',
        });
    }, [client]);

    const close_engine = React.useCallback(() => {
        send_console_command('quit');
        window.setTimeout(() => window.close(), 180);
    }, [send_console_command]);

    const apply_scene_doc = React.useCallback((doc: SceneDocument) => {
        const id = seq.current++;
        const next = { ...doc, path };
        pending_scene_actions.current.set(id, 'sync');
        client.send<SceneCommandRequest>('scene', 'command', {
            id,
            action: 'sync',
            path,
            name: next.name,
            document_json: scene_text(next),
        });
    }, [client, path]);

    const publish_selection = React.useCallback((entity: SceneEntity, index: number) => {
        set_selected_entity(entity.name);
        client.deliver({
            v: PROTOCOL_VERSION,
            ch: 'schemas',
            type: 'catalog',
            payload: { components: SCENE_SCHEMAS },
        });
        client.deliver({
            v: PROTOCOL_VERSION,
            ch: 'selection',
            type: 'state',
            payload: selection_for_entity(entity, index),
        });
    }, [client]);

    const publish_hierarchy_selection = React.useCallback((
        doc: SceneDocument,
        names: string[],
        primary_name: string | null,
    ) => {
        if (names.length > 1) {
            set_selected_entity(primary_name);
            client.deliver({
                v: PROTOCOL_VERSION,
                ch: 'schemas',
                type: 'catalog',
                payload: { components: SCENE_SCHEMAS },
            });
            client.deliver({
                v: PROTOCOL_VERSION,
                ch: 'selection',
                type: 'state',
                payload: multi_selection_for_scene_entities(doc, names),
            });
            return;
        }
        if (!primary_name) {
            client.deliver({
                v: PROTOCOL_VERSION,
                ch: 'selection',
                type: 'cleared',
                payload: null,
            });
            set_selected_entity(null);
            return;
        }
        const index = doc.entities.findIndex((entity) => entity.name === primary_name);
        if (index >= 0) {
            publish_selection(doc.entities[index], index);
        }
    }, [client, publish_selection]);

    const publish_doc_selection = React.useCallback((doc: SceneDocument) => {
        const next_hierarchy_selection = default_selection_for_scene(
            doc,
            hierarchy_selection_ref.current,
        );
        const selected_name = doc.selected_entity && next_hierarchy_selection.includes(doc.selected_entity)
            ? doc.selected_entity
            : next_hierarchy_selection[0] ?? null;
        set_selected_entity(selected_name);
        set_hierarchy_selection(next_hierarchy_selection);
        hierarchy_selection_ref.current = next_hierarchy_selection;
        if (!selected_name) {
            hierarchy_selection_anchor.current = null;
        } else if (
            !hierarchy_selection_anchor.current ||
            !next_hierarchy_selection.includes(hierarchy_selection_anchor.current)
        ) {
            hierarchy_selection_anchor.current = selected_name;
        }
        if (next_hierarchy_selection.length > 1) {
            publish_hierarchy_selection(doc, next_hierarchy_selection, selected_name);
            return;
        }
        const index = selected_name
            ? doc.entities.findIndex((entity) => entity.name === selected_name)
            : -1;
        if (index >= 0) {
            publish_selection(doc.entities[index], index);
        } else {
            client.deliver({
                v: PROTOCOL_VERSION,
                ch: 'selection',
                type: 'cleared',
                payload: null,
            });
        }
    }, [client, publish_hierarchy_selection, publish_selection]);

    const publish_cleared_selection = React.useCallback(() => {
        set_selected_entity(null);
        set_hierarchy_selection([]);
        hierarchy_selection_anchor.current = null;
        client.deliver({
            v: PROTOCOL_VERSION,
            ch: 'selection',
            type: 'cleared',
            payload: null,
        });
    }, [client]);

    const install_scene_doc = React.useCallback((doc: SceneDocument, next_dirty: boolean) => {
        scene_ref.current = doc;
        set_scene(doc);
        set_editor_text(scene_text(doc));
        set_json_error(null);
        set_dirty(next_dirty);
        publish_doc_selection(doc);
    }, [publish_doc_selection]);

    const restore_scene_doc = React.useCallback((doc: SceneDocument) => {
        const next = clone_scene(doc);
        install_scene_doc(next, true);
        apply_scene_doc(next);
    }, [apply_scene_doc, install_scene_doc]);

    const undo_scene = React.useCallback(() => {
        const history = history_ref.current;
        const previous = history.past[history.past.length - 1];
        if (!previous) return;
        history.past = history.past.slice(0, -1);
        history.future = [clone_scene(scene_ref.current), ...history.future].slice(0, HISTORY_LIMIT);
        publish_history_counts();
        restore_scene_doc(previous);
    }, [publish_history_counts, restore_scene_doc]);

    const redo_scene = React.useCallback(() => {
        const history = history_ref.current;
        const next = history.future[0];
        if (!next) return;
        history.future = history.future.slice(1);
        history.past = [...history.past, clone_scene(scene_ref.current)].slice(-HISTORY_LIMIT);
        publish_history_counts();
        restore_scene_doc(next);
    }, [publish_history_counts, restore_scene_doc]);

    React.useEffect(() => {
        const unsub = client.subscribe('scene', (env: Envelope) => {
            if (env.type !== 'command_result') return;
            const result = env.payload as SceneCommandResult;
            const next_status = status_from_payload(result.status);
            const action = pending_scene_actions.current.get(result.id);
            pending_scene_actions.current.delete(result.id);
            if (is_scene_io_action(action)) {
                set_active_scene_action((current_action) => (
                    current_action === action ? null : current_action
                ));
            }
            set_status(next_status);
            set_scene_error(result.ok ? null : result.text || next_status.message || 'scene command failed');
            const is_lifecycle_echo = action === 'status' || is_scene_io_action(action);
            if (!next_status.document_json && is_lifecycle_echo) {
                set_dirty(next_status.dirty);
                if (next_status.path) {
                    set_path(next_status.path);
                }
            }
            if (next_status.document_json) {
                const parsed_doc = parse_scene_json(next_status.document_json);
                const next_path = next_status.path || parsed_doc.path || DEFAULT_PATH;
                const doc = { ...parsed_doc, path: next_path };
                const current = scene_ref.current;
                const changed = !same_scene(current, doc);
                const is_local_sync_echo = action === 'sync';
                const is_passive_refresh = is_lifecycle_echo;
                const sync_message = next_status.message || result.text || '';
                const is_engine_gizmo_begin = result.id === 0 && sync_message === 'gizmo drag begin';
                const is_engine_gizmo_update = result.id === 0 && sync_message === 'gizmo drag update';
                const is_engine_gizmo_commit = result.id === 0 && sync_message === 'gizmo drag commit';
                const is_engine_live_sync =
                    is_engine_gizmo_begin || is_engine_gizmo_update || is_engine_gizmo_commit;
                if ((is_engine_gizmo_begin || is_engine_gizmo_update) &&
                    !engine_gizmo_history_base.current) {
                    engine_gizmo_history_base.current = clone_scene(current);
                }
                if (changed &&
                    !is_local_sync_echo &&
                    !is_passive_refresh &&
                    !is_engine_live_sync &&
                    result.id !== 0) {
                    push_history(current);
                }
                if (is_engine_gizmo_commit) {
                    const base = engine_gizmo_history_base.current;
                    engine_gizmo_history_base.current = null;
                    if (base && !same_scene(base, doc)) {
                        push_history(base);
                    }
                }
                const next_dirty = is_passive_refresh
                    ? next_status.dirty
                    : next_status.dirty || changed || is_local_sync_echo;
                set_path(next_path);
                install_scene_doc(doc, next_dirty);
            }
        });
        return unsub;
    }, [client, install_scene_doc, push_history]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') {
                const id = seq.current++;
                pending_scene_actions.current.set(id, 'status');
                client.send<SceneCommandRequest>('scene', 'command', {
                    id,
                    action: 'status',
                    path: '',
                    name: '',
                    document_json: '',
                });
            }
        });
        return unsub;
    }, [client]);

    React.useEffect(() => {
        client.deliver({
            v: PROTOCOL_VERSION,
            ch: 'schemas',
            type: 'catalog',
            payload: { components: SCENE_SCHEMAS },
        });
    }, [client]);

    React.useEffect(() => {
        const on_key_down = (event: KeyboardEvent) => {
            const target = event.target as HTMLElement | null;
            if (target?.closest('input, textarea, [contenteditable="true"]')) {
                return;
            }
            const command = event.metaKey || event.ctrlKey;
            if (!command) return;
            const key = event.key.toLowerCase();
            if (key === 'z') {
                event.preventDefault();
                if (event.shiftKey) {
                    redo_scene();
                } else {
                    undo_scene();
                }
            } else if (key === 'y' && event.ctrlKey) {
                event.preventDefault();
                redo_scene();
            }
        };
        window.addEventListener('keydown', on_key_down);
        return () => window.removeEventListener('keydown', on_key_down);
    }, [redo_scene, undo_scene]);

    React.useEffect(() => set_scene_patch_handler((patch) => {
        const cur = scene_ref.current;
        const index = resolve_scene_patch_entity_index(cur, patch);
        if (index < 0 || index >= cur.entities.length) {
            return false;
        }
        const current = cur.entities[index];
        const next_entity = apply_selection_patch_to_entity(current, patch);
        if (next_entity === current) {
            return false;
        }
        const next_entities = cur.entities.slice();
        next_entities[index] = next_entity;
        const next = { ...cur, entities: next_entities };
        push_history(cur);
        install_scene_doc(next, true);
        apply_scene_doc(next);
        return true;
    }), [apply_scene_doc, install_scene_doc, push_history]);

    const commit_scene_doc = (
        next: SceneDocument,
        selection_name_or_options: string | null | SceneCommitOptions = selected_entity,
    ) => {
        const current = scene_ref.current;
        const options: SceneCommitOptions = typeof selection_name_or_options === 'object' &&
            selection_name_or_options !== null
            ? selection_name_or_options
            : {
                label: 'scene edit',
                selectionName: selection_name_or_options,
            };
        const next_selection = options.selectionName ?? null;
        const with_selection = { ...next, selected_entity: next_selection };
        const hierarchy_names = default_selection_for_scene(
            with_selection,
            options.hierarchySelection ?? hierarchy_selection_ref.current,
        );
        const primary = next_selection && hierarchy_names.includes(next_selection)
            ? next_selection
            : hierarchy_names[0] ?? null;
        const committed = { ...with_selection, selected_entity: primary };
        const hierarchy_changed = !same_string_list(hierarchy_selection_ref.current, hierarchy_names);
        if (same_scene(current, committed) && !hierarchy_changed) {
            return;
        }
        last_transaction_label.current = options.label;
        hierarchy_selection_ref.current = hierarchy_names;
        set_hierarchy_selection(hierarchy_names);
        hierarchy_selection_anchor.current = primary;
        if (same_scene(current, committed)) {
            publish_hierarchy_selection(committed, hierarchy_names, primary);
            return;
        }
        push_history(current);
        install_scene_doc(committed, true);
        apply_scene_doc(committed);
    };

    const update_name = (value: string) => {
        commit_scene_doc({ ...scene_ref.current, name: value }, {
            label: 'rename scene',
            selectionName: selected_entity,
        });
    };

    const add_camera = () => {
        const cur = scene_ref.current;
        const name = unique_entity_name(cur.entities, 'Camera');
        const next_camera: SceneEntity = {
            name,
            components: ['Transform', 'Camera'],
            position: [0.0, 1.4, 4.2],
            rotation_euler_deg: [-8.0, 180.0, 0.0],
            fov_y_deg: 70.0,
        };
        const next = {
            ...cur,
            active_camera: name,
            entities: [...cur.entities, next_camera],
        };
        add_menu_ref.current?.removeAttribute('open');
        commit_scene_doc(next, {
            label: 'create camera',
            selectionName: name,
            hierarchySelection: [name],
        });
    };

    const add_player_start = () => {
        const cur = scene_ref.current;
        const name = unique_entity_name(cur.entities, 'PlayerStart');
        const transform = player_start_transform(cur);
        const next_entity: SceneEntity = {
            name,
            components: ['Transform', 'PlayerStart'],
            position: transform.position,
            rotation_euler_deg: transform.rotation_euler_deg,
            scale: [1.0, 1.0, 1.0],
        };
        const next = {
            ...cur,
            entities: [...cur.entities, next_entity],
        };
        add_menu_ref.current?.removeAttribute('open');
        commit_scene_doc(next, {
            label: 'create player start',
            selectionName: name,
            hierarchySelection: [name],
        });
    };

    const add_fps_pawn = () => {
        const cur = scene_ref.current;
        const name = unique_entity_name(cur.entities, 'FPSPawn');
        const transform = player_start_transform(cur);
        const next_entity: PlayerControllerEntity = {
            name,
            components: ['Transform', 'PlayerStart', 'PlayerController'],
            position: transform.position,
            rotation_euler_deg: transform.rotation_euler_deg,
            scale: [1.0, 1.0, 1.0],
            player_controller: { ...DEFAULT_PLAYER_CONTROLLER },
        };
        const next = {
            ...cur,
            entities: [...cur.entities, next_entity],
        };
        add_menu_ref.current?.removeAttribute('open');
        commit_scene_doc(next, {
            label: 'create fps pawn',
            selectionName: name,
            hierarchySelection: [name],
        });
    };

    const update_text = (text: string) => {
        set_editor_text(text);
        set_dirty(true);
        const doc = parse_scene_json_strict(text);
        if (!doc) {
            set_json_error('Scene JSON is invalid. The last valid scene remains active.');
            return;
        }
        set_json_error(null);
        set_scene_error(null);
        set_scene(doc);
        scene_ref.current = doc;
        set_path(doc.path || path);
    };

    const add_primitive = (kind: PrimitiveKind) => {
        const cur = scene_ref.current;
        const name = unique_entity_name(
            cur.entities,
            kind.charAt(0).toUpperCase() + kind.slice(1),
        );
        const next_entity: SceneEntity = {
            name,
            components: ['Transform', 'Renderable', 'PrimitiveMesh'],
            primitive: kind,
            position: [0.0, kind === 'plane' ? 0.0 : 0.5, 0.0],
            rotation_euler_deg: [0.0, 0.0, 0.0],
            scale: kind === 'plane' ? [4.0, 1.0, 4.0] : [1.0, 1.0, 1.0],
        };
        const next = {
            ...cur,
            entities: [...cur.entities, next_entity],
        };
        add_menu_ref.current?.removeAttribute('open');
        commit_scene_doc(next, {
            label: `create ${kind}`,
            selectionName: name,
            hierarchySelection: [name],
        });
    };

    const add_blockout_preset = (preset: BlockoutPreset) => {
        const cur = scene_ref.current;
        const specs: Record<BlockoutPreset, {
            base: string;
            primitive: PrimitiveKind;
            position: Vec3;
            rotation_euler_deg: Vec3;
            scale: Vec3;
            material: Required<SceneSolidMaterial>;
        }> = {
            floor: {
                base: 'Floor',
                primitive: 'plane',
                position: [0.0, 0.0, 0.0],
                rotation_euler_deg: [0.0, 0.0, 0.0],
                scale: [12.0, 1.0, 12.0],
                material: { ...DEFAULT_SOLID_MATERIAL, albedo: [0.25, 0.28, 0.32], roughness: 0.85 },
            },
            wall: {
                base: 'Wall',
                primitive: 'cube',
                position: [0.0, 1.5, -4.0],
                rotation_euler_deg: [0.0, 0.0, 0.0],
                scale: [6.0, 3.0, 0.25],
                material: { ...DEFAULT_SOLID_MATERIAL, albedo: [0.44, 0.48, 0.56], roughness: 0.72 },
            },
            block: {
                base: 'Block',
                primitive: 'cube',
                position: [0.0, 0.5, 0.0],
                rotation_euler_deg: [0.0, 0.0, 0.0],
                scale: [1.0, 1.0, 1.0],
                material: { ...DEFAULT_SOLID_MATERIAL, albedo: [0.58, 0.65, 0.72], roughness: 0.62 },
            },
            ramp: {
                base: 'Ramp',
                primitive: 'cube',
                position: [0.0, 0.35, 1.8],
                rotation_euler_deg: [-16.0, 0.0, 0.0],
                scale: [2.8, 0.35, 2.4],
                material: { ...DEFAULT_SOLID_MATERIAL, albedo: [0.52, 0.60, 0.48], roughness: 0.68 },
            },
            cover: {
                base: 'Cover',
                primitive: 'cube',
                position: [1.8, 0.6, 0.0],
                rotation_euler_deg: [0.0, 0.0, 0.0],
                scale: [1.8, 1.2, 0.45],
                material: { ...DEFAULT_SOLID_MATERIAL, albedo: [0.62, 0.55, 0.48], roughness: 0.74 },
            },
        };
        const spec = specs[preset];
        const name = unique_entity_name(cur.entities, spec.base);
        const next_entity: SceneEntity = {
            name,
            components: ['Transform', 'Renderable', 'PrimitiveMesh'],
            primitive: spec.primitive,
            position: spec.position,
            rotation_euler_deg: spec.rotation_euler_deg,
            scale: spec.scale,
            material: spec.material,
        };
        const next = {
            ...cur,
            entities: [...cur.entities, next_entity],
        };
        add_menu_ref.current?.removeAttribute('open');
        commit_scene_doc(next, {
            label: `create ${preset}`,
            selectionName: name,
            hierarchySelection: [name],
        });
    };

    const remove_entity = (name: string) => {
        const cur = scene_ref.current;
        const next_hierarchy_selection = hierarchy_selection_ref.current.filter((selected_name) => (
            selected_name !== name
        ));
        const next_selected = cur.selected_entity === name
            ? next_hierarchy_selection[0] ?? null
            : cur.selected_entity ?? null;
        const entities = cur.entities.filter((e) => e.name !== name);
        const next = {
            ...cur,
            entities,
            active_camera: cur.active_camera === name ? null : cur.active_camera,
            selected_entity: next_selected,
        };
        if (hierarchy_selection_anchor.current === name) {
            hierarchy_selection_anchor.current = next_selected;
        }
        commit_scene_doc(next, {
            label: 'remove entity',
            selectionName: next_selected,
            hierarchySelection: next_hierarchy_selection,
        });
    };

    const selected_entity_names = () => {
        const existing = new Set(scene_ref.current.entities.map((entity) => entity.name));
        const names = hierarchy_selection.filter((name) => existing.has(name));
        if (names.length > 0) {
            return names;
        }
        return selected_entity && existing.has(selected_entity) ? [selected_entity] : [];
    };

    const delete_selected_entities = () => {
        const names = selected_entity_names();
        if (names.length === 0) return;
        const cur = scene_ref.current;
        const removed = new Set(names);
        const next = {
            ...cur,
            entities: cur.entities.filter((entity) => !removed.has(entity.name)),
            active_camera: cur.active_camera && removed.has(cur.active_camera)
                ? null
                : cur.active_camera,
            selected_entity: null,
        };
        commit_scene_doc(next, {
            label: names.length === 1 ? 'delete entity' : 'delete entities',
            selectionName: null,
            hierarchySelection: [],
        });
    };

    const duplicate_entities = (names: string[]) => {
        if (names.length === 0) return;
        const cur = scene_ref.current;
        const selected_names = new Set(names);
        const next_entities: SceneEntity[] = [];
        const duplicate_names: string[] = [];
        for (const entity of cur.entities) {
            next_entities.push(entity);
            if (!selected_names.has(entity.name)) {
                continue;
            }
            const duplicate_name = unique_entity_name(
                [...next_entities, ...cur.entities],
                `${entity.name}_Copy`,
            );
            duplicate_names.push(duplicate_name);
            next_entities.push(offset_duplicate_entity({
                ...clone_entity(entity),
                name: duplicate_name,
            }, duplicate_names.length));
        }
        const primary = duplicate_names[0] ?? null;
        if (!primary) return;
        const next = {
            ...cur,
            entities: next_entities,
            selected_entity: primary,
        };
        commit_scene_doc(next, {
            label: duplicate_names.length === 1 ? 'duplicate entity' : 'duplicate entities',
            selectionName: primary,
            hierarchySelection: duplicate_names,
        });
    };

    const duplicate_selected_entities = () => {
        duplicate_entities(selected_entity_names());
    };

    const clear_scene_selection = () => {
        publish_cleared_selection();
        const cur = scene_ref.current;
        commit_scene_doc({ ...cur, selected_entity: null }, {
            label: 'clear selection',
            selectionName: null,
            hierarchySelection: [],
        });
    };

    const select_primary_entity = (name: string | null, publish = true) => {
        if (!name) {
            clear_scene_selection();
            return;
        }
        const cur = scene_ref.current;
        const index = cur.entities.findIndex((entity) => entity.name === name);
        if (index < 0) {
            clear_scene_selection();
            return;
        }
        if (publish) {
            publish_selection(cur.entities[index], index);
        } else {
            set_selected_entity(name);
        }
        commit_scene_doc({ ...cur, selected_entity: name }, {
            label: 'select entity',
            selectionName: name,
            hierarchySelection: [name],
        });
    };

    const select_entity = (
        entity: SceneEntity,
        index: number,
        event?: React.MouseEvent,
    ) => {
        if (renaming_entity) {
            return;
        }
        const entity_names = scene_ref.current.entities.map((candidate) => candidate.name);
        const additive = Boolean(event?.metaKey || event?.ctrlKey);
        const range = Boolean(event?.shiftKey);
        const anchor_name = hierarchy_selection_anchor.current ?? selected_entity ?? entity.name;
        const anchor_index = entity_names.indexOf(anchor_name);
        let next_selection: string[];
        if (range && anchor_index >= 0) {
            const start = Math.min(anchor_index, index);
            const end = Math.max(anchor_index, index);
            const range_names = entity_names.slice(start, end + 1);
            next_selection = additive
                ? entity_names.filter((name) => (
                    hierarchy_selection.includes(name) || range_names.includes(name)
                ))
                : range_names;
        } else if (additive) {
            const selected_names = new Set(hierarchy_selection);
            if (selected_names.has(entity.name)) {
                selected_names.delete(entity.name);
            } else {
                selected_names.add(entity.name);
            }
            next_selection = entity_names.filter((name) => selected_names.has(name));
        } else {
            next_selection = [entity.name];
        }
        if (!range) {
            hierarchy_selection_anchor.current = entity.name;
        }
        set_hierarchy_selection(next_selection);
        const primary = next_selection.includes(entity.name)
            ? entity.name
            : next_selection[next_selection.length - 1] ?? null;
        commit_scene_doc({ ...scene_ref.current, selected_entity: primary }, {
            label: next_selection.length > 1 ? 'select entities' : 'select entity',
            selectionName: primary,
            hierarchySelection: next_selection,
        });
    };

    const begin_rename_entity = (name: string) => {
        set_renaming_entity(name);
        set_rename_draft(name);
        const cur = scene_ref.current;
        const index = cur.entities.findIndex((entity) => entity.name === name);
        if (index >= 0) {
            set_hierarchy_selection([name]);
            hierarchy_selection_anchor.current = name;
            select_primary_entity(name);
        }
    };

    const cancel_rename_entity = () => {
        set_renaming_entity(null);
        set_rename_draft('');
    };

    const commit_entity_rename = (name: string, value: string) => {
        const trimmed = value.trim();
        if (!trimmed || trimmed === name) {
            cancel_rename_entity();
            return;
        }
        const cur = scene_ref.current;
        const target = cur.entities.find((entity) => entity.name === name);
        if (!target) {
            cancel_rename_entity();
            return;
        }
        const used = new Set(cur.entities
            .filter((entity) => entity.name !== name)
            .map((entity) => entity.name));
        const next_name = used.has(trimmed)
            ? unique_entity_name(cur.entities.filter((entity) => entity.name !== name), trimmed)
            : trimmed;
        const next_entities = cur.entities.map((entity) => (
            entity.name === name ? { ...entity, name: next_name } : entity
        ));
        const next = {
            ...cur,
            entities: next_entities,
            active_camera: cur.active_camera === name ? next_name : cur.active_camera,
            selected_entity: next_name,
        };
        set_hierarchy_selection((current) => current.map((selected_name) => (
            selected_name === name ? next_name : selected_name
        )));
        if (hierarchy_selection_anchor.current === name) {
            hierarchy_selection_anchor.current = next_name;
        }
        set_renaming_entity(null);
        set_rename_draft('');
        commit_scene_doc(next, {
            label: 'rename entity',
            selectionName: next_name,
            hierarchySelection: hierarchy_selection_ref.current.map((selected_name) => (
                selected_name === name ? next_name : selected_name
            )),
        });
    };

    const rename_selected = (value: string) => {
        if (!selected) return;
        commit_entity_rename(selected.name, value);
    };

    const update_editor_settings = (
        patch: Partial<SceneEditorSettings>,
        label = 'update editor setting',
    ) => {
        const cur = scene_ref.current;
        const next = {
            ...cur,
            editorSettings: {
                ...(cur.editorSettings ?? {}),
                ...editor_settings(cur),
                ...patch,
            },
        };
        commit_scene_doc(next, {
            label,
            selectionName: selected_entity,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const set_transform_mode = (next_mode: TransformMode) => {
        if (next_mode === transform_mode) return;
        update_editor_settings({ transform_mode: next_mode }, 'set transform mode');
    };

    const toggle_grid_visible = (visible: boolean) => {
        update_editor_settings({ grid_visible: visible }, 'toggle placement grid');
    };

    const set_render_debug = (mode: RenderDebugMode) => {
        if (mode === render_debug) return;
        update_editor_settings({ render_debug: mode }, 'set render debug');
    };

    const set_active_camera = (name: string) => {
        const cur = scene_ref.current;
        const entity = cur.entities.find((candidate) => candidate.name === name);
        if (!entity?.components.includes('Camera') || cur.active_camera === name) {
            return;
        }
        commit_scene_doc({ ...cur, active_camera: name }, {
            label: 'set active camera',
            selectionName: selected_entity,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_clear_color_hex = (hex: string) => {
        const rgb = hex_to_rgb01(hex);
        if (!rgb) return;
        const cur = scene_ref.current;
        const [, , , a] = clear_color(cur);
        const next = {
            ...cur,
            environmentSettings: {
                ...cur.environmentSettings,
                clear_color: [rgb[0], rgb[1], rgb[2], a] as [number, number, number, number],
            },
        };
        commit_scene_doc(next, {
            label: 'update clear color',
            selectionName: selected_entity,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_clear_channel = (index: 0 | 1 | 2 | 3, value: string) => {
        const parsed = clamp01(Number(value));
        const cur = scene_ref.current;
        const next_color = clear_color(cur);
        next_color[index] = parsed;
        const next = {
            ...cur,
            environmentSettings: {
                ...cur.environmentSettings,
                clear_color: next_color,
            },
        };
        commit_scene_doc(next, {
            label: 'update clear color channel',
            selectionName: selected_entity,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_time_of_day = (value: string) => {
        const cur = scene_ref.current;
        const time_of_day_hours = clamp_range(Number(value), 0.0, 24.0, 12.0);
        commit_scene_doc({
            ...cur,
            environmentSettings: {
                ...cur.environmentSettings,
                sun: {
                    ...(cur.environmentSettings.sun ?? {}),
                    time_of_day_hours,
                },
            },
        }, {
            label: 'update time of day',
            selectionName: selected_entity,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_sun_intensity = (value: string) => {
        const cur = scene_ref.current;
        const intensity_lux = clamp_range(Number(value), 0.0, 200_000.0, 100_000.0);
        commit_scene_doc({
            ...cur,
            environmentSettings: {
                ...cur.environmentSettings,
                sun: {
                    ...(cur.environmentSettings.sun ?? {}),
                    intensity_lux,
                },
            },
        }, {
            label: 'update sun intensity',
            selectionName: selected_entity,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_exposure = (value: string) => {
        const cur = scene_ref.current;
        const exposure = clamp_range(Number(value), -8.0, 8.0, 0.0);
        commit_scene_doc({
            ...cur,
            environmentSettings: {
                ...cur.environmentSettings,
                lighting: {
                    ...(cur.environmentSettings.lighting ?? {}),
                    exposure,
                },
            },
        }, {
            label: 'update exposure',
            selectionName: selected_entity,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_cloud_coverage = (value: string) => {
        const cur = scene_ref.current;
        const coverage = clamp01(Number(value));
        commit_scene_doc({
            ...cur,
            environmentSettings: {
                ...cur.environmentSettings,
                clouds: {
                    ...(cur.environmentSettings.clouds ?? {}),
                    coverage,
                },
            },
        }, {
            label: 'update cloud coverage',
            selectionName: selected_entity,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_cloud_speed = (value: string) => {
        const cur = scene_ref.current;
        const speed = clamp_range(Number(value), -1.0, 1.0, DEFAULT_CLOUD_SPEED);
        commit_scene_doc({
            ...cur,
            environmentSettings: {
                ...cur.environmentSettings,
                clouds: {
                    ...(cur.environmentSettings.clouds ?? {}),
                    speed,
                },
            },
        }, {
            label: 'update cloud speed',
            selectionName: selected_entity,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_selected_material_rgb = (key: MaterialRgbKey, hex: string) => {
        const rgb = hex_to_rgb01(hex);
        if (!rgb || !selected) return;
        const cur = scene_ref.current;
        const index = cur.entities.findIndex((entity) => entity.name === selected.name);
        if (index < 0) return;
        const next_entities = cur.entities.slice();
        const current = next_entities[index];
        next_entities[index] = {
            ...current,
            material: {
                ...solid_material(current),
                ...(current.material ?? {}),
                [key]: rgb,
            },
        };
        commit_scene_doc({
            ...cur,
            entities: next_entities,
        }, {
            label: `update material ${key}`,
            selectionName: selected.name,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_selected_material_number = (
        key: 'roughness' | 'metallic' | 'emissive_intensity',
        value: string,
    ) => {
        if (!selected) return;
        const cur = scene_ref.current;
        const index = cur.entities.findIndex((entity) => entity.name === selected.name);
        if (index < 0) return;
        const next_entities = cur.entities.slice();
        const current = next_entities[index];
        const next_value = key === 'emissive_intensity'
            ? clamp_range(Number(value), 0.0, 64.0, DEFAULT_SOLID_MATERIAL.emissive_intensity)
            : clamp01(Number(value));
        next_entities[index] = {
            ...current,
            material: {
                ...solid_material(current),
                ...(current.material ?? {}),
                [key]: next_value,
            },
        };
        commit_scene_doc({
            ...cur,
            entities: next_entities,
        }, {
            label: `update material ${key}`,
            selectionName: selected.name,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const update_selected_player_controller_number = (
        key: PlayerControllerKey,
        value: string,
    ) => {
        if (!selected) return;
        const parsed = Number(value);
        if (!Number.isFinite(parsed)) return;
        const cur = scene_ref.current;
        const index = cur.entities.findIndex((entity) => entity.name === selected.name);
        if (index < 0) return;
        const next_entities = cur.entities.slice();
        const current = next_entities[index] as PlayerControllerEntity;
        const merged = {
            ...DEFAULT_PLAYER_CONTROLLER,
            ...player_controller(current),
            [key]: parsed,
        };
        next_entities[index] = {
            ...current,
            components: current.components.includes('PlayerController')
                ? current.components
                : [...current.components, 'PlayerController'],
            player_controller: player_controller({
                ...current,
                player_controller: merged,
            } as PlayerControllerEntity),
        } as SceneEntity;
        commit_scene_doc({
            ...cur,
            entities: next_entities,
        }, {
            label: `update player controller ${key}`,
            selectionName: selected.name,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    const add_player_controller_to_selected = () => {
        if (!selected) return;
        const cur = scene_ref.current;
        const index = cur.entities.findIndex((entity) => entity.name === selected.name);
        if (index < 0) return;
        const next_entities = cur.entities.slice();
        const current = next_entities[index] as PlayerControllerEntity;
        next_entities[index] = {
            ...current,
            components: current.components.includes('PlayerController')
                ? current.components
                : [...current.components, 'PlayerController'],
            player_controller: {
                ...DEFAULT_PLAYER_CONTROLLER,
                ...(current.player_controller ?? {}),
            },
        } as SceneEntity;
        commit_scene_doc({
            ...cur,
            entities: next_entities,
        }, {
            label: 'add player controller',
            selectionName: selected.name,
            hierarchySelection: hierarchy_selection_ref.current,
        });
    };

    React.useEffect(() => {
        const on_key_down = (event: KeyboardEvent) => {
            const target = event.target as HTMLElement | null;
            if (target?.closest('input, textarea, [contenteditable="true"]')) {
                return;
            }
            const command = event.metaKey || event.ctrlKey;
            if (command && event.key.toLowerCase() === 'd' && selected_entity_names().length > 0) {
                event.preventDefault();
                duplicate_selected_entities();
                return;
            }
            if (event.metaKey || event.ctrlKey || event.altKey) {
                return;
            }
            const key = event.key.toLowerCase();
            const next_mode =
                key === 'w' ? 'translate' :
                key === 'e' ? 'rotate' :
                key === 'r' ? 'scale' :
                null;
            if (event.key === 'F2' && selected_entity) {
                event.preventDefault();
                begin_rename_entity(selected_entity);
                return;
            }
            if (event.key === 'Delete' && selected_entity_names().length > 0) {
                event.preventDefault();
                delete_selected_entities();
                return;
            }
            if (!next_mode) return;
            event.preventDefault();
            set_transform_mode(next_mode);
        };
        window.addEventListener('keydown', on_key_down);
        return () => window.removeEventListener('keydown', on_key_down);
    }, [hierarchy_selection, selected_entity, transform_mode]);

    const is_settings = mode === 'settings';
    const env_color = clear_color(scene);
    const time_of_day_hours = clamp_range(
        finite_number(scene.environmentSettings.sun?.time_of_day_hours, 12.0),
        0.0,
        24.0,
        12.0,
    );
    const sun_intensity_lux = clamp_range(
        finite_number(scene.environmentSettings.sun?.intensity_lux, 100_000.0),
        0.0,
        200_000.0,
        100_000.0,
    );
    const exposure = clamp_range(
        finite_number(scene.environmentSettings.lighting?.exposure, 0.0),
        -8.0,
        8.0,
        0.0,
    );
    const cloud_coverage = clamp01(
        finite_number(scene.environmentSettings.clouds?.coverage, 0.35),
    );
    const cloud_speed = clamp_range(
        finite_number(scene.environmentSettings.clouds?.speed, DEFAULT_CLOUD_SPEED),
        -1.0,
        1.0,
        DEFAULT_CLOUD_SPEED,
    );
    const selected_position = selected
        ? vec3_value(selected.position, [0.0, 0.0, 0.0])
        : null;
    const selected_material = solid_material(selected);
    const selected_has_material = Boolean(
        selected?.components.includes('Renderable') ||
        selected?.components.includes('PrimitiveMesh'),
    );
    const selected_controller = player_controller(selected);
    const selected_has_player_controller = Boolean(
        selected?.components.includes('PlayerController'),
    );
    const selected_can_have_player_controller = Boolean(
        selected?.components.includes('PlayerStart') ||
        selected?.components.includes('Transform'),
    );
    const hierarchy_filter_query = hierarchy_filter.trim().toLowerCase();
    const filtered_entities = hierarchy_filter_query
        ? scene.entities
            .map((entity, index) => ({ entity, index }))
            .filter(({ entity }) => {
                const primitive = entity.primitive ?? '';
                const type = entity_kind_label(entity);
                return `${entity.name} ${primitive} ${type} ${entity.components.join(' ')}`
                    .toLowerCase()
                    .includes(hierarchy_filter_query);
            })
        : scene.entities.map((entity, index) => ({ entity, index }));
    const scene_message = active_scene_action
        ? action_progress_label(active_scene_action)
        : (json_error ?? scene_error ?? status.message) || (has_dirty_scene ? 'unsaved changes' : 'ready');
    const scene_message_class = json_error || scene_error
        ? 'is-bad'
        : active_scene_action
            ? 'is-busy'
            : has_dirty_scene
                ? 'is-dirty'
                : status.has_camera
                    ? 'is-ok'
                    : 'is-warn';
    const scene_dirty_label = active_scene_action
        ? 'busy'
        : has_dirty_scene
            ? 'unsaved'
            : 'saved';

    return (
        <div className="psy-panel psy-scene" aria-busy={busy}>
            <header className="psy-panel-header">
                <h2>{title}</h2>
                <ConnectionBadge />
                <div className="psy-scene-actions" role="toolbar" aria-label="Play controls">
                    <button
                        type="button"
                        className="psy-btn psy-btn-primary"
                        onClick={() => send_console_command('play')}
                        title="Start play mode"
                    >
                        play
                    </button>
                    <button
                        type="button"
                        className="psy-btn"
                        onClick={() => send_console_command('stop')}
                        title="Stop play mode"
                    >
                        stop
                    </button>
                    <button
                        type="button"
                        className="psy-btn"
                        onClick={close_engine}
                        title="Quit the engine and close this editor window"
                    >
                        quit
                    </button>
                </div>
                {is_settings && (
                <div className="psy-scene-actions">
                    <button
                        type="button"
                        className="psy-btn"
                        disabled={busy}
                        onClick={() => send_scene('create')}
                        title={has_dirty_scene ? 'Create a new scene and replace unsaved local edits' : 'Create a new scene'}
                    >
                        {active_scene_action === 'create' ? 'creating...' : 'create'}
                    </button>
                    <button
                        type="button"
                        className="psy-btn"
                        disabled={busy}
                        onClick={() => send_scene('load')}
                        title={has_dirty_scene ? 'Load from disk and replace unsaved local edits' : 'Load scene from disk'}
                    >
                        {active_scene_action === 'load' ? 'loading...' : 'load'}
                    </button>
                    <button
                        type="button"
                        className="psy-btn psy-btn-primary"
                        disabled={busy || Boolean(json_error)}
                        onClick={() => send_scene('save')}
                        title={json_error ? 'Fix scene JSON before saving' : 'Save scene to disk'}
                    >
                        {active_scene_action === 'save' ? 'saving...' : 'save'}
                    </button>
                </div>
                )}
            </header>

            <div className={`psy-scene-body is-${mode}`}>
                {!is_settings && (
                <section className="psy-scene-main" aria-label="Hierarchy">
                    <div className="psy-scene-section">
                        <span>add entity</span>
                        <div className="psy-scene-primitive-actions">
                            <details className="psy-scene-add-menu" ref={add_menu_ref}>
                                <summary className="psy-btn" title="Create entity">
                                    + create
                                </summary>
                                <div className="psy-scene-add-popover" role="menu">
                                    <button
                                        type="button"
                                        role="menuitem"
                                        onClick={add_camera}
                                    >
                                        <b>Camera</b>
                                        <span>active view</span>
                                    </button>
                                    <button
                                        type="button"
                                        role="menuitem"
                                        onClick={add_player_start}
                                    >
                                        <b>Player Start</b>
                                        <span>spawn point</span>
                                    </button>
                                    <button
                                        type="button"
                                        role="menuitem"
                                        onClick={add_fps_pawn}
                                    >
                                        <b>FPS Pawn</b>
                                        <span>spawn + controller</span>
                                    </button>
                                    {BLOCKOUT_PRESETS.map((preset) => (
                                        <button
                                            key={preset}
                                            type="button"
                                            role="menuitem"
                                            onClick={() => add_blockout_preset(preset)}
                                        >
                                            <b>{preset}</b>
                                            <span>fps blockout</span>
                                        </button>
                                    ))}
                                    {PRIMITIVES.map((kind) => (
                                        <button
                                            key={kind}
                                            type="button"
                                            role="menuitem"
                                            onClick={() => add_primitive(kind)}
                                        >
                                            <b>{kind}</b>
                                            <span>primitive mesh</span>
                                        </button>
                                    ))}
                                </div>
                            </details>
                            <div
                                className="psy-scene-history-actions"
                                role="toolbar"
                                aria-label="Fast FPS blockout creation"
                            >
                                {BLOCKOUT_PRESETS.map((preset) => (
                                    <button
                                        key={preset}
                                        type="button"
                                        className="psy-btn"
                                        onClick={() => add_blockout_preset(preset)}
                                        title={`Create ${preset} blockout piece`}
                                    >
                                        {preset}
                                    </button>
                                ))}
                                <button
                                    type="button"
                                    className="psy-btn psy-btn-primary"
                                    onClick={add_fps_pawn}
                                    title="Create a PlayerStart with PlayerController tuning"
                                >
                                    fps pawn
                                </button>
                            </div>
                        </div>
                    </div>

                    <div className="psy-scene-edit-tools">
                        <div className="psy-scene-outliner-head">
                            <span>edit mode</span>
                            <code>
                                {hierarchy_selection.length > 1
                                    ? `${hierarchy_selection.length} selected`
                                    : selected ? selected.name : 'no selection'}
                            </code>
                        </div>
                        <div
                            className="psy-scene-history-actions"
                            data-history={`${history_counts.past}:${history_counts.future}`}
                        >
                            <button
                                type="button"
                                className="psy-btn"
                                disabled={!can_undo}
                                onClick={undo_scene}
                                title="Undo scene change"
                            >
                                undo
                            </button>
                            <button
                                type="button"
                                className="psy-btn"
                                disabled={!can_redo}
                                onClick={redo_scene}
                                title="Redo scene change"
                            >
                                redo
                            </button>
                        </div>
                        <div
                            className="psy-scene-history-actions"
                            role="toolbar"
                            aria-label="Transform mode"
                        >
                            {TRANSFORM_MODES.map((tool) => (
                                <button
                                    key={tool}
                                    type="button"
                                    className="psy-btn"
                                    data-selected={transform_mode === tool ? 'true' : 'false'}
                                    aria-pressed={transform_mode === tool}
                                    onClick={() => set_transform_mode(tool)}
                                    title={`${tool} tool (${tool === 'translate' ? 'W' : tool === 'rotate' ? 'E' : 'R'})`}
                                >
                                    {tool}
                                </button>
                            ))}
                        </div>
                        <div
                            className="psy-scene-history-actions"
                            role="toolbar"
                            aria-label="Selection operations"
                        >
                            <button
                                type="button"
                                className="psy-btn"
                                disabled={selected_entity_names().length === 0}
                                onClick={duplicate_selected_entities}
                                title="Duplicate selected entities"
                            >
                                {selected_entity_names().length > 1 ? `duplicate ${selected_entity_names().length}` : 'duplicate'}
                            </button>
                            <button
                                type="button"
                                className="psy-btn"
                                disabled={selected_entity_names().length === 0}
                                onClick={delete_selected_entities}
                                title="Delete selected entities"
                            >
                                {selected_entity_names().length > 1 ? `delete ${selected_entity_names().length}` : 'delete'}
                            </button>
                            <button
                                type="button"
                                className="psy-btn"
                                disabled={!selected_entity && hierarchy_selection.length === 0}
                                onClick={clear_scene_selection}
                                title="Clear hierarchy selection"
                            >
                                clear
                            </button>
                        </div>
                        <div className="psy-scene-grid-row">
                            <label>
                                <input
                                    type="checkbox"
                                    checked={grid_visible}
                                    onChange={(e) => toggle_grid_visible(e.target.checked)}
                                />
                                show placement grid
                            </label>
                            <code>{grid_visible ? 'visible' : 'hidden'}</code>
                        </div>
                        {has_render_debug_setting && (
                        <div
                            className="psy-scene-history-actions"
                            role="toolbar"
                            aria-label="Render debug"
                        >
                            {RENDER_DEBUG_MODES.map((mode_name) => (
                                <button
                                    key={mode_name}
                                    type="button"
                                    className="psy-btn"
                                    data-selected={render_debug === mode_name ? 'true' : 'false'}
                                    aria-pressed={render_debug === mode_name}
                                    onClick={() => set_render_debug(mode_name)}
                                    title={`Render debug: ${mode_name}`}
                                >
                                    {mode_name === 'off' ? 'lit' : mode_name}
                                </button>
                            ))}
                        </div>
                        )}
                        <div className="psy-scene-grid-row">
                            <span>viewport camera</span>
                            <code>{viewport_camera}</code>
                        </div>
                        {selected && selected_position ? (
                            <>
                                <label className="psy-scene-field">
                                    <span>name</span>
                                    <input
                                        className="psy-input"
                                        defaultValue={selected.name}
                                        key={selected.name}
                                        onBlur={(e) => rename_selected(e.target.value)}
                                        onKeyDown={(e) => {
                                            if (e.key === 'Enter') {
                                                e.currentTarget.blur();
                                            }
                                        }}
                                        spellCheck={false}
                                    />
                                </label>
                                <div className="psy-scene-position-readout">
                                    <span data-axis="x">x {display_num(selected_position[0])}</span>
                                    <span data-axis="y">y {display_num(selected_position[1])}</span>
                                    <span data-axis="z">z {display_num(selected_position[2])}</span>
                                </div>
                                {selected_has_player_controller ? (
                                    <div className="psy-scene-env-card">
                                        <div className="psy-scene-outliner-head">
                                            <span>pawn controller</span>
                                            <code>fps</code>
                                        </div>
                                        <div className="psy-scene-rgba">
                                            <label>
                                                <span>radius</span>
                                                <CommitNumberInput
                                                    value={selected_controller.capsule_radius_m}
                                                    decimals={2}
                                                    min={0.05}
                                                    max={2.0}
                                                    step={0.01}
                                                    ariaLabel="Capsule radius"
                                                    onCommit={(value) => update_selected_player_controller_number('capsule_radius_m', value)}
                                                />
                                            </label>
                                            <label>
                                                <span>height</span>
                                                <CommitNumberInput
                                                    value={selected_controller.capsule_height_m}
                                                    decimals={2}
                                                    min={0.2}
                                                    max={4.0}
                                                    step={0.01}
                                                    ariaLabel="Capsule height"
                                                    onCommit={(value) => update_selected_player_controller_number('capsule_height_m', value)}
                                                />
                                            </label>
                                            <label>
                                                <span>eye</span>
                                                <CommitNumberInput
                                                    value={selected_controller.eye_height_m}
                                                    decimals={2}
                                                    min={0.1}
                                                    max={4.0}
                                                    step={0.01}
                                                    ariaLabel="Eye height"
                                                    onCommit={(value) => update_selected_player_controller_number('eye_height_m', value)}
                                                />
                                            </label>
                                        </div>
                                        <div className="psy-scene-rgba">
                                            <label>
                                                <span>walk</span>
                                                <CommitNumberInput
                                                    value={selected_controller.walk_speed_mps}
                                                    decimals={1}
                                                    min={0.0}
                                                    max={20.0}
                                                    step={0.1}
                                                    ariaLabel="Walk speed"
                                                    onCommit={(value) => update_selected_player_controller_number('walk_speed_mps', value)}
                                                />
                                            </label>
                                            <label>
                                                <span>run</span>
                                                <CommitNumberInput
                                                    value={selected_controller.run_speed_mps}
                                                    decimals={1}
                                                    min={0.0}
                                                    max={30.0}
                                                    step={0.1}
                                                    ariaLabel="Run speed"
                                                    onCommit={(value) => update_selected_player_controller_number('run_speed_mps', value)}
                                                />
                                            </label>
                                            <label>
                                                <span>jump</span>
                                                <CommitNumberInput
                                                    value={selected_controller.jump_speed_mps}
                                                    decimals={1}
                                                    min={0.0}
                                                    max={20.0}
                                                    step={0.1}
                                                    ariaLabel="Jump speed"
                                                    onCommit={(value) => update_selected_player_controller_number('jump_speed_mps', value)}
                                                />
                                            </label>
                                        </div>
                                        <label className="psy-scene-field">
                                            <span>mouse sensitivity</span>
                                            <CommitNumberInput
                                                value={selected_controller.mouse_sensitivity}
                                                decimals={2}
                                                min={0.01}
                                                max={2.0}
                                                step={0.01}
                                                ariaLabel="Mouse sensitivity"
                                                onCommit={(value) => update_selected_player_controller_number('mouse_sensitivity', value)}
                                            />
                                        </label>
                                    </div>
                                ) : selected_can_have_player_controller ? (
                                    <button
                                        type="button"
                                        className="psy-btn"
                                        onClick={add_player_controller_to_selected}
                                        title="Add forward-compatible FPS controller JSON to this entity"
                                    >
                                        add pawn controller
                                    </button>
                                ) : null}
                            </>
                        ) : (
                            <div className="psy-empty">Select an entity to move it.</div>
                        )}
                    </div>

                    <div className="psy-scene-outliner">
                        <div className="psy-scene-outliner-head">
                            <span>outliner</span>
                            <code>
                                {hierarchy_filter_query
                                    ? `${filtered_entities.length}/${scene.entities.length}`
                                    : (status.has_scene ? 'open' : 'local')}
                            </code>
                        </div>
                        <label className="psy-scene-field">
                            <span>search</span>
                            <input
                                className="psy-input"
                                value={hierarchy_filter}
                                onChange={(e) => set_hierarchy_filter(e.target.value)}
                                onKeyDown={(e) => {
                                    if (e.key === 'Escape') {
                                        set_hierarchy_filter('');
                                        e.currentTarget.blur();
                                    }
                                }}
                                placeholder="name, camera, cube..."
                                spellCheck={false}
                            />
                        </label>
                        <div
                            className="psy-scene-entity-list"
                            onClick={(e) => {
                                if (e.target === e.currentTarget) {
                                    clear_scene_selection();
                                }
                            }}
                        >
                            {filtered_entities.map(({ entity, index }) => (
                                <div
                                    key={entity.name}
                                    className="psy-scene-entity-row"
                                    data-selected={
                                        selected_entity === entity.name ||
                                        hierarchy_selection.includes(entity.name)
                                            ? 'true'
                                            : 'false'
                                    }
                                    data-multiselected={
                                        hierarchy_selection.length > 1 &&
                                        hierarchy_selection.includes(entity.name)
                                            ? 'true'
                                            : undefined
                                    }
                                    data-primary={selected_entity === entity.name ? 'true' : undefined}
                                    data-active-camera={scene.active_camera === entity.name ? 'true' : undefined}
                                    onDoubleClick={() => begin_rename_entity(entity.name)}
                                >
                                    <button
                                        type="button"
                                        className="psy-scene-entity-pick"
                                        onClick={(e) => select_entity(entity, index, e)}
                                        title="Select entity. Shift selects a range, Cmd/Ctrl toggles."
                                    >
                                        <span className="psy-scene-entity-icon" aria-hidden="true">
                                            {entity_kind_icon(entity)}
                                        </span>
                                        <span className="psy-scene-entity-main">
                                            {renaming_entity === entity.name ? (
                                                <input
                                                    ref={rename_input_ref}
                                                    className="psy-input psy-scene-rename-input"
                                                    value={rename_draft}
                                                    onChange={(e) => set_rename_draft(e.target.value)}
                                                    onClick={(e) => e.stopPropagation()}
                                                    onDoubleClick={(e) => e.stopPropagation()}
                                                    onBlur={() => commit_entity_rename(entity.name, rename_draft)}
                                                    onKeyDown={(e) => {
                                                        e.stopPropagation();
                                                        if (e.key === 'Enter') {
                                                            commit_entity_rename(entity.name, rename_draft);
                                                        } else if (e.key === 'Escape') {
                                                            cancel_rename_entity();
                                                        }
                                                    }}
                                                    spellCheck={false}
                                                />
                                            ) : (
                                                <b>{entity.name}</b>
                                            )}
                                            <span>
                                                {entity_detail_label(entity)}
                                                {entity.components.includes('Camera') &&
                                                 scene.active_camera === entity.name
                                                    ? ' / active'
                                                    : ''}
                                            </span>
                                        </span>
                                    </button>
                                    <button
                                        type="button"
                                        className="psy-btn"
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            begin_rename_entity(entity.name);
                                        }}
                                        title="Rename entity"
                                    >
                                        rename
                                    </button>
                                    <button
                                        type="button"
                                        className="psy-btn"
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            duplicate_entities([entity.name]);
                                        }}
                                        title="Duplicate entity"
                                    >
                                        copy
                                    </button>
                                    <button
                                        type="button"
                                        className="psy-btn"
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            remove_entity(entity.name);
                                        }}
                                        title="Delete entity"
                                    >
                                        delete
                                    </button>
                                    {entity.components.includes('Camera') && (
                                    <button
                                        type="button"
                                        className="psy-btn"
                                        disabled={scene.active_camera === entity.name}
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            set_active_camera(entity.name);
                                        }}
                                        title="Set active scene camera"
                                    >
                                        {scene.active_camera === entity.name ? 'active' : 'set active'}
                                    </button>
                                    )}
                                </div>
                            ))}
                            {filtered_entities.length === 0 && (
                                <div className="psy-empty">No entities match the filter.</div>
                            )}
                        </div>
                    </div>
                </section>
                )}

                {is_settings && (
                <section className="psy-scene-main psy-scene-settings" aria-label="Scene settings">
                    <div className="psy-scene-topology">
                        <div>
                            <span>scene</span>
                            <b>{scene.name || DEFAULT_NAME}</b>
                        </div>
                        <div>
                            <span>path</span>
                            <b>{path}</b>
                        </div>
                        <div>
                            <span>camera</span>
                            <b>{scene.active_camera ?? 'none'}</b>
                        </div>
                        <div>
                            <span>state</span>
                            <b>{status.has_scene ? 'open' : 'local'}</b>
                        </div>
                        <div data-state={scene_dirty_label}>
                            <span>disk</span>
                            <b>{scene_dirty_label}</b>
                        </div>
                    </div>

                    <div className={`psy-scene-message ${scene_message_class}`}>
                        {scene_message}
                    </div>

                    <div className="psy-scene-env-card">
                        <div className="psy-scene-outliner-head">
                            <span>environment</span>
                            <code>clear</code>
                        </div>
                        <label className="psy-scene-color">
                            <span>clear color</span>
                            <input
                                type="color"
                                value={clear_color_hex(scene)}
                                onChange={(e) => update_clear_color_hex(e.target.value)}
                            />
                            <code>{clear_color_hex(scene)}</code>
                        </label>
                        <div className="psy-scene-rgba">
                            {(['r', 'g', 'b', 'a'] as const).map((channel, i) => (
                                <label key={channel}>
                                    <span>{channel}</span>
                                    <input
                                        className="psy-input"
                                        type="number"
                                        min="0"
                                        max="1"
                                        step="0.001"
                                        value={env_color[i].toFixed(3)}
                                        onChange={(e) => update_clear_channel(i as 0 | 1 | 2 | 3, e.target.value)}
                                    />
                                </label>
                            ))}
                        </div>
                        <label className="psy-scene-field">
                            <span>sky mode</span>
                            <input
                                className="psy-input"
                                value={scene.environmentSettings.sky_mode ?? ''}
                                onChange={(e) => {
                                    const cur = scene_ref.current;
                                    commit_scene_doc({
                                        ...cur,
                                        environmentSettings: {
                                            ...cur.environmentSettings,
                                            sky_mode: e.target.value,
                                        },
                                    }, {
                                        label: 'update sky mode',
                                        selectionName: selected_entity,
                                        hierarchySelection: hierarchy_selection_ref.current,
                                    });
                                }}
                            />
                        </label>
                        <div className="psy-scene-outliner-head">
                            <span>sun / sky</span>
                            <code>{display_num(time_of_day_hours)}h</code>
                        </div>
                        <div className="psy-scene-rgba">
                            <label>
                                <span>time</span>
                                <input
                                    className="psy-input"
                                    type="number"
                                    min="0"
                                    max="24"
                                    step="0.1"
                                    value={time_of_day_hours.toFixed(1)}
                                    onChange={(e) => update_time_of_day(e.target.value)}
                                />
                            </label>
                            <label>
                                <span>sun lux</span>
                                <input
                                    className="psy-input"
                                    type="number"
                                    min="0"
                                    max="200000"
                                    step="1000"
                                    value={sun_intensity_lux.toFixed(0)}
                                    onChange={(e) => update_sun_intensity(e.target.value)}
                                />
                            </label>
                            <label>
                                <span>exposure</span>
                                <input
                                    className="psy-input"
                                    type="number"
                                    min="-8"
                                    max="8"
                                    step="0.05"
                                    value={exposure.toFixed(2)}
                                    onChange={(e) => update_exposure(e.target.value)}
                                />
                            </label>
                        </div>
                        <div className="psy-scene-outliner-head">
                            <span>clouds</span>
                            <code>{display_num(cloud_coverage * 100)}%</code>
                        </div>
                        <div className="psy-scene-rgba">
                            <label>
                                <span>coverage</span>
                                <input
                                    className="psy-input"
                                    type="number"
                                    min="0"
                                    max="1"
                                    step="0.01"
                                    value={cloud_coverage.toFixed(2)}
                                    onChange={(e) => update_cloud_coverage(e.target.value)}
                                />
                            </label>
                            <label>
                                <span>speed</span>
                                <input
                                    className="psy-input"
                                    type="number"
                                    min="-1"
                                    max="1"
                                    step="0.005"
                                    value={cloud_speed.toFixed(3)}
                                    onChange={(e) => update_cloud_speed(e.target.value)}
                                />
                            </label>
                        </div>
                    </div>

                    <div className="psy-scene-env-card">
                        <div className="psy-scene-outliner-head">
                            <span>selected material</span>
                            <code>{selected_has_material && selected ? selected.name : 'none'}</code>
                        </div>
                        {selected_has_material && selected ? (
                            <>
                                <label className="psy-scene-color">
                                    <span>albedo</span>
                                    <input
                                        type="color"
                                        value={rgb_hex(selected_material.albedo)}
                                        onChange={(e) => update_selected_material_rgb('albedo', e.target.value)}
                                    />
                                    <code>{rgb_hex(selected_material.albedo)}</code>
                                </label>
                                <div className="psy-scene-rgba">
                                    <label>
                                        <span>rough</span>
                                        <input
                                            className="psy-input"
                                            type="number"
                                            min="0"
                                            max="1"
                                            step="0.01"
                                            value={selected_material.roughness.toFixed(2)}
                                            onChange={(e) => update_selected_material_number('roughness', e.target.value)}
                                        />
                                    </label>
                                    <label>
                                        <span>metal</span>
                                        <input
                                            className="psy-input"
                                            type="number"
                                            min="0"
                                            max="1"
                                            step="0.01"
                                            value={selected_material.metallic.toFixed(2)}
                                            onChange={(e) => update_selected_material_number('metallic', e.target.value)}
                                        />
                                    </label>
                                </div>
                                <label className="psy-scene-color">
                                    <span>emissive</span>
                                    <input
                                        type="color"
                                        value={rgb_hex(selected_material.emissive)}
                                        onChange={(e) => update_selected_material_rgb('emissive', e.target.value)}
                                    />
                                    <code>{rgb_hex(selected_material.emissive)}</code>
                                </label>
                                <label className="psy-scene-field">
                                    <span>emissive power</span>
                                    <input
                                        className="psy-input"
                                        type="number"
                                        min="0"
                                        max="64"
                                        step="0.1"
                                        value={selected_material.emissive_intensity.toFixed(1)}
                                        onChange={(e) => update_selected_material_number('emissive_intensity', e.target.value)}
                                    />
                                </label>
                            </>
                        ) : (
                            <div className="psy-empty">Select a renderable primitive to edit solid material.</div>
                        )}
                    </div>
                </section>
                )}

                {is_settings && (
                <aside className="psy-scene-side">
                    <label className="psy-scene-field">
                        <span>name</span>
                        <input
                            className="psy-input"
                            value={scene.name}
                            onChange={(e) => update_name(e.target.value)}
                        />
                    </label>
                    <label className="psy-scene-field">
                        <span>path</span>
                        <input
                            className="psy-input"
                            value={path}
                            onChange={(e) => {
                                set_path(e.target.value);
                                set_dirty(true);
                            }}
                            spellCheck={false}
                        />
                    </label>
                    <ul className="psy-scene-stats">
                        <li><span>state</span><code>{status.has_scene ? 'open' : 'empty'}</code></li>
                        <li><span>entities</span><code>{scene.entities.length}</code></li>
                        <li><span>camera</span><code>{scene.active_camera ?? 'none'}</code></li>
                        <li><span>disk</span><code>{scene_dirty_label}</code></li>
                    </ul>

                    <div className={`psy-scene-message ${scene_message_class}`}>
                        {scene_message}
                    </div>

                    <textarea
                        className="psy-scene-json"
                        value={editor_text}
                        onChange={(e) => update_text(e.target.value)}
                        spellCheck={false}
                    />
                </aside>
                )}
            </div>
        </div>
    );
}
