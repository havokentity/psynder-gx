// SPDX-License-Identifier: MIT
// Tiny in-process bridge between hierarchy/scene authoring and inspector
// panels. The engine remains the persistence authority; this bus only lets
// docked web panels share live authoring edits without routing through mocks.

import type {
    ComponentSchema,
    MultiSelectionState,
    SceneDocument,
    SceneEntity,
    SelectionPatch,
    SelectionState,
} from '../../ipc/protocol';

export type ScenePatchSource = NonNullable<SelectionPatch['source']>;
export type SceneVec3 = [number, number, number];
export type SceneTransformField = 'position' | 'rotation_euler_deg' | 'scale';

export interface SceneSelectionPatch extends SelectionPatch {
    entity_id: number;
    entity_label?: string;
    entity_key?: string;
    component: string;
    field: string;
    value: unknown;
    source?: ScenePatchSource;
    commit?: boolean;
}

type PatchHandler = (patch: SceneSelectionPatch) => boolean;

let patch_handler: PatchHandler | null = null;

export function set_scene_patch_handler(handler: PatchHandler): () => void {
    patch_handler = handler;
    return () => {
        if (patch_handler === handler) {
            patch_handler = null;
        }
    };
}

export function apply_scene_selection_patch(patch: SceneSelectionPatch): boolean {
    return patch_handler ? patch_handler(patch) : false;
}

export function scene_entity_key(entity: Pick<SceneEntity, 'name'>, index: number): string {
    const name = entity.name.trim();
    return name ? `name:${name}` : `id:${index + 1}`;
}

export function selection_key(selection: SelectionState | null): string {
    if (!selection) return 'none';
    if (selection.entity_key) return selection.entity_key;
    const label = selection.entity_label?.trim();
    return label ? `name:${label}` : `id:${selection.entity_id}`;
}

export function selection_identity(selection: SelectionState | null): string {
    return selection_key(selection);
}

export function selection_matches_patch(
    selection: SelectionState | null,
    patch: Pick<SceneSelectionPatch, 'entity_id' | 'entity_label' | 'entity_key'>,
): selection is SelectionState {
    if (!selection) return false;
    if (patch.entity_key && selection_key(selection) !== patch.entity_key) return false;
    if (patch.entity_label && selection.entity_label) {
        return patch.entity_label === selection.entity_label;
    }
    return selection.entity_id === patch.entity_id;
}

export function make_scene_selection_patch(
    selection: SelectionState,
    component: string,
    field: string,
    value: unknown,
    source: ScenePatchSource,
    commit = true,
): SceneSelectionPatch {
    return {
        entity_id: selection.entity_id,
        entity_label: selection.entity_label,
        entity_key: selection_key(selection),
        component,
        field,
        value,
        source,
        commit,
    };
}

export function is_transform_field(field: string): field is SceneTransformField {
    return field === 'position' || field === 'rotation_euler_deg' || field === 'scale';
}

export function is_scene_vec3(value: unknown): value is SceneVec3 {
    return Array.isArray(value)
        && value.length >= 3
        && value.slice(0, 3).every((n) => typeof n === 'number' && Number.isFinite(n));
}

export function make_transform_patch(
    selection: SelectionState,
    field: SceneTransformField,
    value: SceneVec3,
    source: ScenePatchSource,
    commit = true,
): SceneSelectionPatch {
    return make_scene_selection_patch(selection, 'Transform', field, value, source, commit);
}

export function resolve_scene_patch_entity_index(
    scene: SceneDocument,
    patch: Pick<SceneSelectionPatch, 'entity_id' | 'entity_label' | 'entity_key'>,
): number {
    const keyed_name = patch.entity_key?.startsWith('name:')
        ? patch.entity_key.slice('name:'.length)
        : '';
    const target_name = patch.entity_label || keyed_name;
    if (target_name) {
        const named = scene.entities.findIndex((entity) => entity.name === target_name);
        if (named >= 0) return named;
    }
    const index = patch.entity_id - 1;
    if (index < 0 || index >= scene.entities.length) return -1;
    if (target_name && scene.entities[index]?.name !== target_name) return -1;
    return index;
}

export const LOOSE_SCENE_SCHEMAS: ComponentSchema[] = [
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
];

function is_scene_document(value: unknown): value is SceneDocument {
    if (!value || typeof value !== 'object') return false;
    const rec = value as Partial<SceneDocument>;
    return Array.isArray(rec.entities);
}

export function parse_loose_scene_document(text: string): SceneDocument | null {
    try {
        const parsed = JSON.parse(text) as unknown;
        return is_scene_document(parsed) ? parsed : null;
    } catch {
        return null;
    }
}

export function selection_for_scene_entity(
    entity: SceneEntity,
    index: number,
): SelectionState {
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
    return {
        entity_id: index + 1,
        entity_label: entity.name,
        entity_key: scene_entity_key(entity, index),
        components,
    };
}

export function selection_from_scene_document(
    scene: SceneDocument,
): SelectionState | null {
    const selected_name = scene.selected_entity ?? null;
    if (!selected_name) return null;
    const index = scene.entities.findIndex((entity) => entity.name === selected_name);
    if (index < 0) return null;
    return selection_for_scene_entity(scene.entities[index], index);
}

export function multi_selection_for_scene_entities(
    scene: SceneDocument,
    names: readonly string[],
): MultiSelectionState {
    const selected_entities = names
        .map((name) => {
            const index = scene.entities.findIndex((entity) => entity.name === name);
            if (index < 0) return null;
            const entity = scene.entities[index];
            return {
                entity_id: index + 1,
                entity_label: entity.name,
                entity_key: scene_entity_key(entity, index),
                components: [...entity.components],
            };
        })
        .filter((item): item is NonNullable<typeof item> => item !== null);
    const component_sets = selected_entities
        .map((item) => new Set(item.components ?? []));
    const common_components = component_sets.length === 0
        ? []
        : [...component_sets[0]].filter((component) => (
            component_sets.every((set) => set.has(component))
        ));
    return {
        kind: 'multi',
        selected_entities,
        primary_entity: selected_entities[0],
        common_components,
    };
}
