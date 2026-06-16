// SPDX-License-Identifier: MIT
// Psynder editor — Inspector panel. Subscribes to the `schemas` channel for
// the PSYNDER_COMPONENT registry plus `selection` for the currently-selected
// entity. Each component on the selection is rendered as a SchemaForm; user
// edits are pushed back to the engine on the same channel as `set` envelopes.

import React from 'react';

import { get_client } from '../ipc/client';
import type {
    ComponentSchema,
    ComponentValueMap,
    Envelope,
    FieldSchema,
    MultiSelectionState,
    SchemaCatalog,
    SchemaDelta,
    SceneCommandResult,
    SelectionPatch,
    SelectionState,
} from '../ipc/protocol';
import { SchemaForm } from '../schema/Form';
import { ConnectionBadge } from './shared/ConnectionBadge';
import {
    LOOSE_SCENE_SCHEMAS,
    apply_scene_selection_patch,
    make_scene_selection_patch,
    parse_loose_scene_document,
    selection_from_scene_document,
    selection_identity,
    selection_matches_patch,
} from './shared/sceneAuthoringBus';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

type Vec3 = [number, number, number];
type TransformFieldName = 'position' | 'rotation_euler_deg' | 'scale';
type InspectorNoticeKind = 'empty' | 'multi' | 'scene' | 'environment' | 'unsupported';

interface InspectorNotice {
    kind: InspectorNoticeKind;
    title: string;
    detail?: string;
    common_components?: string[];
    selected_labels?: string[];
    selected_keys?: string[];
    summary_rows?: Array<[string, string]>;
}

const TRANSFORM_FIELDS: readonly TransformFieldName[] = [
    'position',
    'rotation_euler_deg',
    'scale',
];
const TRANSFORM_FIELD_SET = new Set<string>(TRANSFORM_FIELDS);
const AXES = ['x', 'y', 'z'] as const;

function is_record(value: unknown): value is Record<string, unknown> {
    return !!value && typeof value === 'object' && !Array.isArray(value);
}

function component_value_map(value: unknown): ComponentValueMap | null {
    if (!is_record(value)) return null;
    return value;
}

function component_map(value: unknown): SelectionState['components'] | null {
    if (!is_record(value)) return null;
    const next: SelectionState['components'] = {};
    for (const [name, fields] of Object.entries(value)) {
        const mapped = component_value_map(fields);
        if (!mapped) return null;
        next[name] = mapped;
    }
    return next;
}

function selection_state_from_entityish(value: unknown): SelectionState | null {
    if (!is_record(value)) return null;
    const components = component_map(value.components);
    if (!components || typeof value.entity_id !== 'number') return null;
    return {
        entity_id: value.entity_id,
        entity_label: typeof value.entity_label === 'string' ? value.entity_label : undefined,
        entity_key: typeof value.entity_key === 'string' ? value.entity_key : undefined,
        components,
    };
}

function selection_state_from_payload(payload: unknown): SelectionState | null {
    const direct = selection_state_from_entityish(payload);
    if (direct) return direct;
    if (!is_record(payload)) return null;
    return selection_state_from_entityish(payload.entity)
        ?? selection_state_from_entityish(payload.primary_entity)
        ?? null;
}

function selection_count_hint(payload: Record<string, unknown>): number | null {
    const array_keys = ['selected_entities', 'entity_ids', 'entities'];
    for (const key of array_keys) {
        const value = payload[key];
        if (Array.isArray(value)) return value.length;
    }
    const count = payload.selection_count ?? payload.count;
    return typeof count === 'number' && Number.isFinite(count) ? count : null;
}

function selection_array_from_payload(payload: Record<string, unknown>): unknown[] | null {
    for (const key of ['selected_entities', 'entities', 'entity_ids']) {
        const value = payload[key];
        if (Array.isArray(value)) return value;
    }
    return null;
}

function multiselect_from_payload(payload: Record<string, unknown>): MultiSelectionState | null {
    const selected_entities = selection_array_from_payload(payload);
    if (!selected_entities || selected_entities.length <= 1) return null;
    const items = selected_entities
        .filter(is_record)
        .map((item) => ({
            entity_id: typeof item.entity_id === 'number' ? item.entity_id : 0,
            entity_label: typeof item.entity_label === 'string' ? item.entity_label : undefined,
            entity_key: typeof item.entity_key === 'string' ? item.entity_key : undefined,
            components: component_names_from_entityish(item) ?? undefined,
        }));
    if (items.length === 0) return null;
    return {
        kind: 'multi',
        selected_entities: items,
        primary_entity: is_record(payload.primary_entity)
            ? {
                entity_id: typeof payload.primary_entity.entity_id === 'number'
                    ? payload.primary_entity.entity_id
                    : 0,
                entity_label: typeof payload.primary_entity.entity_label === 'string'
                    ? payload.primary_entity.entity_label
                    : undefined,
                entity_key: typeof payload.primary_entity.entity_key === 'string'
                    ? payload.primary_entity.entity_key
                    : undefined,
                components: component_names_from_entityish(payload.primary_entity) ?? undefined,
            }
            : items[0],
        common_components: Array.isArray(payload.common_components)
            ? payload.common_components.filter((name): name is string => typeof name === 'string')
            : common_components_from_payload(payload),
    };
}

function component_names_from_entityish(value: unknown): string[] | null {
    if (!is_record(value)) return null;

    const components = value.components;
    if (Array.isArray(components)) {
        const names = components.filter((name): name is string => typeof name === 'string');
        return names.length === components.length ? names : null;
    }
    if (is_record(components)) return Object.keys(components);

    const component_names = value.component_names ?? value.componentNames;
    if (Array.isArray(component_names)) {
        const names = component_names.filter((name): name is string => typeof name === 'string');
        return names.length === component_names.length ? names : null;
    }

    return null;
}

function common_components_from_payload(payload: Record<string, unknown>): string[] {
    const selected = selection_array_from_payload(payload);
    if (!selected || selected.length === 0) return [];

    const per_entity = selected.map(component_names_from_entityish);
    if (per_entity.some((names) => names == null)) return [];

    const [first, ...rest] = per_entity as string[][];
    const common = first.filter((name, index) => (
        first.indexOf(name) === index
        && rest.every((names) => names.includes(name))
    ));
    return common.sort((a, b) => a.localeCompare(b));
}

function describe_common_components(components: readonly string[]): string {
    if (components.length === 0) return 'Common components are not inferable from this selection payload.';
    const visible = components.slice(0, 5).join(', ');
    const extra = components.length > 5 ? ` +${components.length - 5} more` : '';
    return `Common components: ${visible}${extra}.`;
}

function lowercase_string(value: unknown): string {
    return typeof value === 'string' ? value.toLowerCase() : '';
}

function notice_from_non_entity_selection(payload: unknown): InspectorNotice | null {
    if (!is_record(payload)) {
        return null;
    }

    const multiselect = multiselect_from_payload(payload);
    if (multiselect) {
        const labels = multiselect.selected_entities
            .map((entity) => entity.entity_label ?? entity.entity_key ?? `#${entity.entity_id}`)
            .filter((label) => label.length > 0);
        const common_components = multiselect.common_components ?? [];
        return {
            kind: 'multi',
            title: `${multiselect.selected_entities.length} entities selected`,
            detail: describe_common_components(common_components),
            common_components,
            selected_labels: labels,
            selected_keys: multiselect.selected_entities
                .map((entity) => entity.entity_key ?? entity.entity_label ?? `id:${entity.entity_id}`),
        };
    }

    const count = selection_count_hint(payload);
    if (count != null && count > 1) {
        const common_components = common_components_from_payload(payload);
        return {
            kind: 'multi',
            title: `${count} entities selected`,
            detail: describe_common_components(common_components),
            common_components,
            selected_labels: selection_array_from_payload(payload)
                ?.map((entity) => {
                    if (!is_record(entity)) return null;
                    return typeof entity.entity_label === 'string'
                        ? entity.entity_label
                        : typeof entity.entity_key === 'string'
                            ? entity.entity_key
                            : null;
                })
                .filter((label): label is string => !!label),
        };
    }
    if (count === 1) return null;

    const kind = lowercase_string(payload.kind)
        || lowercase_string(payload.selection_kind)
        || lowercase_string(payload.target)
        || lowercase_string(payload.scope);
    if (kind.includes('environment')) {
        const env = is_record(payload.environmentSettings)
            ? payload.environmentSettings
            : is_record(payload.environment)
                ? payload.environment
                : payload;
        const rows: Array<[string, string]> = [];
        if (Array.isArray(env.clear_color)) {
            rows.push(['clear color', env.clear_color.slice(0, 4).join(', ')]);
        }
        if (typeof env.sky_mode === 'string') {
            rows.push(['sky mode', env.sky_mode]);
        }
        return {
            kind: 'environment',
            title: 'Environment selected',
            detail: rows.length > 0
                ? 'Scene environment values are available in the Scene panel.'
                : 'Scene environment values are edited from the Scene panel until environment Inspector schemas are available.',
            summary_rows: rows,
        };
    }
    if (kind.includes('scene')) {
        const rows: Array<[string, string]> = [];
        const name = typeof payload.name === 'string' ? payload.name : '';
        const path = typeof payload.path === 'string' ? payload.path : '';
        const entities = selection_count_hint(payload);
        if (name) rows.push(['name', name]);
        if (path) rows.push(['path', path]);
        if (entities != null) rows.push(['entities', String(entities)]);
        return {
            kind: 'scene',
            title: 'Scene selected',
            detail: 'Scene-level settings are edited from the Scene panel.',
            summary_rows: rows,
        };
    }

    if (count == null && kind.length === 0) return null;

    return {
        kind: count === 0 ? 'empty' : 'unsupported',
        title: count === 0 ? 'No entity selected' : 'Selection is not inspectable',
        detail: count === 0
            ? 'Select an entity in the viewport or hierarchy to inspect its components.'
            : 'The current selection is not an entity component selection.',
    };
}

function notice_from_scene_document(document_json: string): InspectorNotice {
    const scene = parse_loose_scene_document(document_json);
    const name = scene?.name?.trim();
    const entity_count = scene?.entities.length ?? 0;
    return {
        kind: 'scene',
        title: name ? `Scene: ${name}` : 'No entity selected',
        detail: entity_count > 0
            ? `${entity_count} scene ${entity_count === 1 ? 'entity' : 'entities'} available. Select one to edit components.`
            : 'Create an entity in the Scene panel to begin authoring.',
    };
}

function same_value(a: unknown, b: unknown): boolean {
    if (Object.is(a, b)) return true;
    if (Array.isArray(a) && Array.isArray(b)) {
        if (a.length !== b.length) return false;
        return a.every((value, i) => Object.is(value, b[i]));
    }
    return false;
}

function same_string_array(a: readonly string[] | undefined, b: readonly string[] | undefined): boolean {
    if (a === b) return true;
    if (!a || !b || a.length !== b.length) return false;
    return a.every((value, index) => value === b[index]);
}

function same_summary_rows(
    a: ReadonlyArray<readonly [string, string]> | undefined,
    b: ReadonlyArray<readonly [string, string]> | undefined,
): boolean {
    if (a === b) return true;
    if (!a || !b || a.length !== b.length) return false;
    return a.every((row, index) => row[0] === b[index][0] && row[1] === b[index][1]);
}

function same_components(
    a: SelectionState['components'],
    b: SelectionState['components'],
): boolean {
    const a_names = Object.keys(a);
    const b_names = Object.keys(b);
    if (a_names.length !== b_names.length) return false;
    return a_names.every((component) => {
        const a_values = a[component];
        const b_values = b[component];
        if (!b_values) return false;
        const a_fields = Object.keys(a_values);
        const b_fields = Object.keys(b_values);
        if (a_fields.length !== b_fields.length) return false;
        return a_fields.every((field) => same_value(a_values[field], b_values[field]));
    });
}

function same_selection(a: SelectionState | null, b: SelectionState | null): boolean {
    if (a === b) return true;
    if (!a || !b) return false;
    return a.entity_id === b.entity_id
        && a.entity_label === b.entity_label
        && a.entity_key === b.entity_key
        && same_components(a.components, b.components);
}

function same_notice(a: InspectorNotice | null, b: InspectorNotice | null): boolean {
    if (a === b) return true;
    if (!a || !b) return false;
    return a.kind === b.kind
        && a.title === b.title
        && a.detail === b.detail
        && same_string_array(a.common_components, b.common_components)
        && same_string_array(a.selected_labels, b.selected_labels)
        && same_string_array(a.selected_keys, b.selected_keys)
        && same_summary_rows(a.summary_rows, b.summary_rows);
}

function is_transform_field_name(name: string): name is TransformFieldName {
    return TRANSFORM_FIELD_SET.has(name);
}

function transform_fallback(field: TransformFieldName): Vec3 {
    return field === 'scale' ? [1, 1, 1] : [0, 0, 0];
}

function vec3_value(value: unknown, fallback: Vec3): Vec3 {
    if (!Array.isArray(value)) return fallback;
    const next: Vec3 = [...fallback];
    for (let i = 0; i < 3; ++i) {
        const n = value[i];
        if (typeof n === 'number' && Number.isFinite(n)) next[i] = n;
    }
    return next;
}

function clamp_numeric(value: number, field: FieldSchema): number {
    const hints = field.numeric;
    if (hints?.min != null && value < hints.min) return hints.min;
    if (hints?.max != null && value > hints.max) return hints.max;
    return value;
}

function format_number(value: number): string {
    if (!Number.isFinite(value)) return '0';
    return String(Object.is(value, -0) ? 0 : value);
}

function component_display_name(name: string): string {
    if (name === 'PrimitiveMesh') return 'Primitive';
    return name;
}

function entity_kind(selection: SelectionState): string {
    const components = selection.components;
    if (components.Camera) return 'Camera';
    const primitive = components.PrimitiveMesh?.primitive;
    if (typeof primitive === 'string' && primitive.trim()) {
        return `${primitive.trim()} primitive`;
    }
    if (components.Renderable) return 'Renderable entity';
    return 'Entity';
}

function entity_summary_rows(selection: SelectionState): Array<[string, string]> {
    const rows: Array<[string, string]> = [['kind', entity_kind(selection)]];
    const camera_fov = selection.components.Camera?.fov_y_deg;
    if (typeof camera_fov === 'number' && Number.isFinite(camera_fov)) {
        rows.push(['camera fov', `${format_number(camera_fov)} deg`]);
    }
    const primitive = selection.components.PrimitiveMesh?.primitive;
    if (typeof primitive === 'string' && primitive.trim()) {
        rows.push(['primitive', primitive.trim()]);
    }
    const visible = selection.components.Renderable?.visible;
    if (typeof visible === 'boolean') {
        rows.push(['visible', visible ? 'yes' : 'no']);
    }
    return rows;
}

export function Inspector() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);

    const [schemas, set_schemas] = React.useState<Map<string, ComponentSchema>>(
        () => new Map(),
    );
    const [selection, set_selection] = React.useState<SelectionState | null>(null);
    const [notice, set_notice] = React.useState<InspectorNotice | null>(null);
    const selection_ref = React.useRef<SelectionState | null>(null);
    const notice_ref = React.useRef<InspectorNotice | null>(null);

    const merge_schemas = React.useCallback((components: ComponentSchema[]) => {
        set_schemas((prev) => {
            const next = new Map(prev);
            for (const component of components) next.set(component.name, component);
            return next;
        });
    }, []);

    const commit_selection = React.useCallback((next: SelectionState | null) => {
        selection_ref.current = next;
        notice_ref.current = null;
        set_selection((prev) => (same_selection(prev, next) ? prev : next));
        if (next) {
            set_notice((prev) => (prev == null ? prev : null));
        }
    }, []);

    const commit_notice = React.useCallback((next: InspectorNotice | null) => {
        selection_ref.current = null;
        notice_ref.current = next;
        set_selection((prev) => (prev == null ? prev : null));
        set_notice((prev) => (same_notice(prev, next) ? prev : next));
    }, []);

    // ── Schemas subscription ────────────────────────────────────────────
    React.useEffect(() => {
        const unsub = client.subscribe('schemas', (env: Envelope) => {
            if (env.type === 'catalog') {
                const cat = env.payload as SchemaCatalog;
                set_schemas(() => {
                    const m = new Map<string, ComponentSchema>();
                    for (const c of cat.components) m.set(c.name, c);
                    return m;
                });
            } else if (env.type === 'delta') {
                const d = env.payload as SchemaDelta;
                set_schemas((prev) => {
                    const m = new Map(prev);
                    for (const c of d.added ?? []) m.set(c.name, c);
                    for (const n of d.removed ?? []) m.delete(n);
                    return m;
                });
            }
        });
        return unsub;
    }, [client]);

    // ── Selection subscription ──────────────────────────────────────────
    React.useEffect(() => {
        const unsub = client.subscribe('selection', (env: Envelope) => {
            if (env.type === 'state') {
                const next = selection_state_from_payload(env.payload);
                if (next) {
                    commit_selection(next);
                } else {
                    const next_notice = notice_from_non_entity_selection(env.payload);
                    if (next_notice) commit_notice(next_notice);
                }
            } else if (env.type === 'patch') {
                const p = env.payload as SelectionPatch;
                const cur = selection_ref.current;
                if (!selection_matches_patch(cur, p)) return;
                commit_selection({
                    ...cur,
                    components: {
                        ...cur.components,
                        [p.component]: {
                            ...(cur.components[p.component] ?? {}),
                            [p.field]: p.value,
                        },
                    },
                });
            } else if (env.type === 'cleared') {
                commit_notice(null);
            }
        });
        return unsub;
    }, [client, commit_notice, commit_selection]);

    // ── Scene sync selection mirror ─────────────────────────────────────
    //
    // Engine viewport picking and hierarchy edits both publish the loose scene
    // document. Mirror the selected entity from that document so Inspector is
    // robust even when the Hierarchy panel is not the component that emitted a
    // `selection` envelope.
    React.useEffect(() => {
        const unsub = client.subscribe('scene', (env: Envelope) => {
            if (env.type !== 'command_result') return;
            const result = env.payload as SceneCommandResult;
            const document_json = result.status?.document_json;
            if (!document_json) return;
            const scene = parse_loose_scene_document(document_json);
            if (!scene) return;
            merge_schemas(LOOSE_SCENE_SCHEMAS);
            const next = selection_from_scene_document(scene);
            const current_notice = notice_ref.current;
            if (
                next &&
                current_notice?.kind === 'multi' &&
                current_notice.selected_keys?.some((key) => (
                    key === selection_identity(next) ||
                    key === next.entity_key ||
                    key === next.entity_label ||
                    key === `id:${next.entity_id}`
                ))
            ) {
                return;
            }
            commit_selection(next);
            if (!next) commit_notice(notice_from_scene_document(document_json));
        });
        return unsub;
    }, [client, commit_notice, commit_selection, merge_schemas]);

    // ── Subscribe-request hint ──────────────────────────────────────────
    //
    // The first time the socket opens we send a `subscribe` envelope so the
    // engine knows this panel wants schema + selection streams. Resilient to
    // multiple opens (auto-reconnect): the engine treats it as idempotent.
    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') {
                client.send('schemas',   'subscribe', {});
                client.send('selection', 'subscribe', {});
                merge_schemas(LOOSE_SCENE_SCHEMAS);
            }
        });
        return unsub;
    }, [client, merge_schemas]);

    const on_field_change = React.useCallback(
        (
            component: string,
            field: string,
            value: unknown,
            expected_identity: string,
        ) => {
            const cur = selection_ref.current;
            if (!cur) return;
            if (selection_identity(cur) !== expected_identity) return;
            // Optimistic local update so the input stays in sync.
            const next: SelectionState = {
                ...cur,
                components: {
                    ...cur.components,
                    [component]: {
                        ...(cur.components[component] ?? {}),
                        [field]: value,
                    },
                },
            };
            if (same_value(cur.components[component]?.[field], value)) return;
            commit_selection(next);
            const patch = make_scene_selection_patch(
                cur,
                component,
                field,
                value,
                'inspector',
                true,
            );
            client.send<SelectionPatch>('selection', 'set', {
                ...patch,
                source: 'inspector',
                commit: true,
            });
            apply_scene_selection_patch(patch);
        },
        [client, commit_selection],
    );

    return (
        <div className="psy-panel psy-inspector">
            <header className="psy-panel-header">
                <h2>Inspector</h2>
                <ConnectionBadge />
            </header>

            {!selection && <InspectorEmpty notice={notice} />}

            {selection && (
                <div className="psy-inspector-body">
                    <div className="psy-entity-banner">
                        <span className="psy-entity-id">#{selection.entity_id}</span>
                        {selection.entity_label && (
                            <span className="psy-entity-label">{selection.entity_label}</span>
                        )}
                        <span className="psy-entity-label">{entity_kind(selection)}</span>
                    </div>

                    <SummaryRows rows={entity_summary_rows(selection)} />

                    <div className="psy-entity-banner" aria-label="Components">
                        {Object.keys(selection.components).map((name) => (
                            <span className="psy-entity-id" key={name}>
                                {component_display_name(name)}
                            </span>
                        ))}
                    </div>

                    {Object.entries(selection.components).map(([name, values]) => {
                        const schema = schemas.get(name);
                        const identity = selection_identity(selection);
                        return (
                            <ComponentBlock
                                key={`${identity}:${name}`}
                                name={name}
                                schema={schema}
                                values={values}
                                on_change={(field, v) => on_field_change(name, field, v, identity)}
                            />
                        );
                    })}

                    {schemas.size === 0 && (
                        <div className="psy-empty psy-empty-warning">
                            No component schemas received yet.
                        </div>
                    )}
                </div>
            )}
        </div>
    );
}

function InspectorEmpty({ notice }: { notice: InspectorNotice | null }) {
    const next = notice ?? {
        kind: 'empty',
        title: 'No entity selected',
        detail: 'Click an entity in the viewport or hierarchy.',
    };
    return (
        <div className="psy-empty" data-selection-kind={next.kind}>
            <strong>{next.title}</strong>
            {next.detail && <p>{next.detail}</p>}
            <SummaryRows rows={next.summary_rows ?? []} />
            {next.common_components && next.common_components.length > 0 && (
                <div className="psy-entity-banner" aria-label="Common components">
                    {next.common_components.map((name) => (
                        <span className="psy-entity-id" key={name}>
                            {component_display_name(name)}
                        </span>
                    ))}
                </div>
            )}
            {next.selected_labels && next.selected_labels.length > 0 && (
                <p>
                    {next.selected_labels.slice(0, 6).join(', ')}
                    {next.selected_labels.length > 6
                        ? ` +${next.selected_labels.length - 6} more`
                        : ''}
                </p>
            )}
        </div>
    );
}

function SummaryRows({ rows }: { rows: ReadonlyArray<readonly [string, string]> }) {
    if (rows.length === 0) return null;
    return (
        <dl className="psy-scene-stats">
            {rows.map(([label, value]) => (
                <React.Fragment key={label}>
                    <dt>{label}</dt>
                    <dd>{value}</dd>
                </React.Fragment>
            ))}
        </dl>
    );
}

interface ComponentBlockProps {
    name: string;
    schema?: ComponentSchema;
    values: ComponentValueMap;
    on_change: (field: string, value: unknown) => void;
}

const ComponentBlock = React.memo(function ComponentBlock({
    name,
    schema,
    values,
    on_change,
}: ComponentBlockProps) {
    const [collapsed, set_collapsed] = React.useState(false);
    return (
        <section className="psy-component">
            <button
                type="button"
                className="psy-component-header"
                onClick={() => set_collapsed((c) => !c)}
                aria-expanded={!collapsed}
            >
                <span className={`psy-disclosure ${collapsed ? 'is-collapsed' : ''}`}>
                    {collapsed ? '▸' : '▾'}
                </span>
                <span className="psy-component-name">{name}</span>
            </button>

            {!collapsed && (
                schema
                    ? (
                        name === 'Transform'
                            ? (
                                <TransformForm
                                    schema={schema}
                                    values={values}
                                    on_change={on_change}
                                />
                            )
                            : <SchemaForm schema={schema} values={values} on_change={on_change} />
                    )
                    : (
                        <div className="psy-empty psy-empty-warning">
                            No schema registered for <code>{name}</code> — values shown raw.
                            <pre className="psy-raw">{JSON.stringify(values, null, 2)}</pre>
                        </div>
                    )
            )}
        </section>
    );
}, (prev, next) => (
    prev.name === next.name &&
    prev.schema === next.schema &&
    prev.values === next.values
));

interface TransformFormProps {
    schema: ComponentSchema;
    values: ComponentValueMap;
    on_change: (field: string, value: unknown) => void;
}

function TransformForm({ schema, values, on_change }: TransformFormProps) {
    const transform_fields = schema.fields.filter((field) => (
        is_transform_field_name(field.name)
        && (field.kind === 'vec3' || Array.isArray(values[field.name]))
    ));
    const other_fields = schema.fields.filter((field) => (
        !transform_fields.some((transform_field) => transform_field.name === field.name)
    ));
    const other_schema = other_fields.length > 0
        ? { ...schema, fields: other_fields }
        : null;

    return (
        <>
            {transform_fields.length > 0 && (
                <div className="psy-form psy-transform-form">
                    {transform_fields.map((field) => {
                        const name = field.name as TransformFieldName;
                        return (
                            <TransformVecRow
                                key={field.name}
                                field={field}
                                label={field.name}
                                value={vec3_value(values[field.name], transform_fallback(name))}
                                on_commit={(next) => on_change(field.name, next)}
                            />
                        );
                    })}
                </div>
            )}
            {other_schema && (
                <SchemaForm
                    schema={other_schema}
                    values={values}
                    on_change={on_change}
                />
            )}
        </>
    );
}

interface TransformVecRowProps {
    field: FieldSchema;
    label: string;
    value: Vec3;
    on_commit: (value: Vec3) => void;
}

function TransformVecRow({ field, label, value, on_commit }: TransformVecRowProps) {
    const commit_axis = React.useCallback((axis: number, next_value: number) => {
        const next: Vec3 = [value[0], value[1], value[2]];
        next[axis] = next_value;
        on_commit(next);
    }, [on_commit, value]);

    return (
        <div className="psy-transform-row">
            <label
                className="psy-field-label"
                title={field.help ?? label}
            >
                {label}
                {field.numeric?.unit
                    ? <span className="psy-field-unit"> ({field.numeric.unit})</span>
                    : null}
            </label>
            <div className="psy-transform-vec" role="group" aria-label={label}>
                {AXES.map((axis, i) => (
                    <label className="psy-transform-axis" key={axis}>
                        <span className="psy-vec-axis">{axis}</span>
                        <TransformNumberInput
                            field={field}
                            value={value[i]}
                            on_commit={(next) => commit_axis(i, next)}
                        />
                    </label>
                ))}
            </div>
        </div>
    );
}

interface TransformNumberInputProps {
    field: FieldSchema;
    value: number;
    on_commit: (value: number) => void;
}

function TransformNumberInput({
    field,
    value,
    on_commit,
}: TransformNumberInputProps) {
    const [draft, set_draft] = React.useState(() => format_number(value));
    const [invalid, set_invalid] = React.useState(false);
    const focused = React.useRef(false);

    React.useEffect(() => {
        if (!focused.current) {
            set_draft(format_number(value));
            set_invalid(false);
        }
    }, [value]);

    const reset = React.useCallback(() => {
        set_draft(format_number(value));
        set_invalid(false);
    }, [value]);

    const commit = React.useCallback((raw: string): boolean => {
        const trimmed = raw.trim();
        const parsed = Number(trimmed);
        if (trimmed === '' || !Number.isFinite(parsed)) {
            set_invalid(true);
            return false;
        }
        const next = clamp_numeric(parsed, field);
        set_invalid(false);
        set_draft(format_number(next));
        if (!Object.is(next, value)) on_commit(next);
        return true;
    }, [field, on_commit, value]);

    const dirty = draft !== format_number(value);

    return (
        <input
            type="text"
            inputMode="decimal"
            className="psy-input psy-input-number psy-transform-number"
            data-dirty={dirty ? 'true' : undefined}
            aria-invalid={invalid ? 'true' : undefined}
            disabled={field.readonly}
            value={draft}
            onFocus={() => { focused.current = true; }}
            onChange={(e) => {
                set_draft(e.target.value);
                if (invalid) set_invalid(false);
            }}
            onBlur={(e) => {
                focused.current = false;
                if (!commit(e.target.value)) reset();
            }}
            onWheel={(e) => {
                e.preventDefault();
                e.stopPropagation();
            }}
            onKeyDown={(e) => {
                if (e.key === 'Enter') {
                    e.preventDefault();
                    commit(e.currentTarget.value);
                } else if (e.key === 'Escape') {
                    e.preventDefault();
                    reset();
                    e.currentTarget.select();
                }
            }}
            spellCheck={false}
        />
    );
}
