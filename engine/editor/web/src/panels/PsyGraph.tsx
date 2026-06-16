// SPDX-License-Identifier: MIT
// Psynder editor — PsyGraph visual behavior authoring panel. Wave C keeps this
// offline/mock-friendly while the backend protocol catches up.

import React from 'react';

import { get_client } from '../ipc/client';
import { mock_psygraph } from '../ipc/mock';
import type {
    Envelope,
    PsyGraphCompileResult,
    PsyGraphDocument,
    PsyGraphNode,
    PsyGraphSaveResult,
    PsyGraphValue,
} from '../ipc/protocol';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

function clone_doc(doc: PsyGraphDocument): PsyGraphDocument {
    return JSON.parse(JSON.stringify(doc)) as PsyGraphDocument;
}

function value_text(value: PsyGraphValue): string {
    if (Array.isArray(value)) return `[${value.map((x) => x.toFixed(2)).join(', ')}]`;
    if (typeof value === 'object') {
        if (value.type === 'linearIndex') return `linear_index(${value.base}, ${value.step})`;
        return `${value.value}`;
    }
    return String(value);
}

function draft_key(id: string): string {
    return `psygx_psygraph_draft_${id}`;
}

function load_draft(doc: PsyGraphDocument): PsyGraphDocument | null {
    try {
        const raw = window.localStorage.getItem(draft_key(doc.id));
        if (!raw) return null;
        const parsed = JSON.parse(raw) as PsyGraphDocument;
        return parsed && parsed.id === doc.id ? parsed : null;
    } catch {
        return null;
    }
}

export function PsyGraph() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);
    const [doc, set_doc] = React.useState<PsyGraphDocument | null>(null);
    const [source_doc, set_source_doc] = React.useState<PsyGraphDocument | null>(null);
    const [selected_id, set_selected_id] = React.useState<string>('spin_crates');
    const [export_open, set_export_open] = React.useState(false);
    const [status, set_status] = React.useState('idle');
    const compile_req = React.useRef<number | null>(null);
    const save_req = React.useRef<number | null>(null);

    React.useEffect(() => {
        const handle = window.setTimeout(() => {
            set_doc((cur) => {
                if (cur) return cur;
                const fallback = clone_doc(mock_psygraph());
                return load_draft(fallback) ?? fallback;
            });
            set_source_doc((cur) => cur ?? clone_doc(load_draft(mock_psygraph()) ?? mock_psygraph()));
        }, 350);
        return () => window.clearTimeout(handle);
    }, []);

    React.useEffect(() => {
        const unsub = client.subscribe('psygraph', (env: Envelope) => {
            if (env.type === 'document') {
                const incoming = clone_doc(env.payload as PsyGraphDocument);
                const next = load_draft(incoming) ?? incoming;
                set_doc(next);
                set_source_doc(clone_doc(next));
                set_selected_id((id) => next.nodes.some((n) => n.id === id) ? id : next.nodes[0]?.id ?? '');
                return;
            }
            if (env.type === 'compile_result') {
                const result = env.payload as PsyGraphCompileResult;
                if (compile_req.current !== result.id) return;
                compile_req.current = null;
                set_status(result.ok ? result.text || 'compiled' : `compile failed: ${result.text}`);
                return;
            }
            if (env.type === 'save_result') {
                const result = env.payload as PsyGraphSaveResult;
                if (save_req.current !== result.id) return;
                save_req.current = null;
                set_status(result.ok ? `saved ${result.path}` : `save failed: ${result.text}`);
            }
        });
        return unsub;
    }, [client]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') client.send('psygraph', 'subscribe', {});
        });
        return unsub;
    }, [client]);

    const selected = doc?.nodes.find((n) => n.id === selected_id) ?? null;
    const exported = React.useMemo(() => doc ? serialize_graph(doc) : '', [doc]);
    const dirty = React.useMemo(() => {
        if (!doc || !source_doc) return false;
        return JSON.stringify(doc) !== JSON.stringify(source_doc);
    }, [doc, source_doc]);

    const update_node_value = React.useCallback((node_id: string, key: string, value: PsyGraphValue) => {
        set_doc((prev) => {
            if (!prev) return prev;
            return {
                ...prev,
                nodes: prev.nodes.map((node) => node.id === node_id
                    ? { ...node, values: { ...node.values, [key]: value } }
                    : node),
            };
        });
    }, []);

    const reset = React.useCallback(() => {
        if (!source_doc) return;
        set_doc(clone_doc(source_doc));
        set_export_open(false);
    }, [source_doc]);

    const save_draft = React.useCallback(() => {
        if (!doc) return;
        window.localStorage.setItem(draft_key(doc.id), JSON.stringify(doc));
        if (client.current_state() !== 'open') {
            set_source_doc(clone_doc(doc));
            set_status('saved local draft');
            return;
        }
        const id = Date.now();
        save_req.current = id;
        set_status('saving');
        client.send('psygraph', 'save', {
            id,
            source_path: doc.source_path,
            document_json: JSON.stringify(doc, null, 2),
        });
        set_source_doc(clone_doc(doc));
    }, [client, doc]);

    const compile_graph = React.useCallback(() => {
        if (!doc) return;
        const id = Date.now();
        compile_req.current = id;
        set_status('compiling');
        client.send('psygraph', 'compile', {
            id,
            graph_json: serialize_graph(doc),
            source_path: doc.source_path,
        });
    }, [client, doc]);

    return (
        <div className="psy-panel psy-graph">
            <header className="psy-panel-header">
                <h2>PsyGraph</h2>
                <ConnectionBadge />
                <span className={`psy-graph-dirty ${dirty ? 'is-dirty' : ''}`}>
                    {dirty ? 'modified' : 'clean'}
                </span>
                <span className="psy-graph-dirty">{status}</span>
                <button
                    type="button"
                    className="psy-btn psy-btn-primary"
                    disabled={!doc}
                    onClick={compile_graph}
                >
                    compile
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    disabled={!doc}
                    onClick={save_draft}
                >
                    save
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    disabled={!doc}
                    onClick={() => set_export_open((open) => !open)}
                >
                    export
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    disabled={!dirty}
                    onClick={reset}
                >
                    revert
                </button>
            </header>

            {!doc ? (
                <div className="psy-empty">Waiting for graph document…</div>
            ) : (
                <div className="psy-graph-body">
                    <GraphCanvas doc={doc} selected_id={selected_id} on_select={set_selected_id} />
                    <Inspector doc={doc} node={selected} on_change={update_node_value} />
                    {export_open && (
                        <ExportDrawer json={exported} on_close={() => set_export_open(false)} />
                    )}
                </div>
            )}
        </div>
    );
}

function GraphCanvas({
    doc,
    selected_id,
    on_select,
}: {
    doc: PsyGraphDocument;
    selected_id: string;
    on_select(id: string): void;
}) {
    return (
        <div className="psy-graph-canvas" role="application" aria-label="PsyGraph canvas">
            <svg className="psy-graph-links" width="100%" height="100%">
                {doc.links.map((link) => {
                    const a = doc.nodes.find((n) => n.id === link.from_node);
                    const b = doc.nodes.find((n) => n.id === link.to_node);
                    if (!a || !b) return null;
                    const x0 = a.x + 180;
                    const y0 = a.y + 42;
                    const x1 = b.x;
                    const y1 = b.y + 42;
                    const mid = (x0 + x1) * 0.5;
                    return (
                        <path
                            key={link.id}
                            d={`M ${x0} ${y0} C ${mid} ${y0}, ${mid} ${y1}, ${x1} ${y1}`}
                            className="psy-graph-link"
                        />
                    );
                })}
            </svg>
            {doc.nodes.map((node) => (
                <button
                    key={node.id}
                    type="button"
                    className={`psy-graph-node ${selected_id === node.id ? 'is-selected' : ''}`}
                    style={{ left: node.x, top: node.y }}
                    onClick={() => on_select(node.id)}
                >
                    <span className="psy-graph-node-title">{node.title}</span>
                    <span className="psy-graph-node-op">{node.op}</span>
                    <PinList node={node} />
                </button>
            ))}
        </div>
    );
}

function PinList({ node }: { node: PsyGraphNode }) {
    const pins = [...node.inputs, ...node.outputs];
    if (pins.length === 0) return null;
    return (
        <span className="psy-graph-pins">
            {pins.slice(0, 4).map((pin, index) => (
                <span key={`${pin.id}-${pin.kind}-${index}`} className={`psy-graph-pin is-${pin.kind}`}>
                    {pin.label}
                </span>
            ))}
        </span>
    );
}

function Inspector({
    doc,
    node,
    on_change,
}: {
    doc: PsyGraphDocument;
    node: PsyGraphNode | null;
    on_change(node_id: string, key: string, value: PsyGraphValue): void;
}) {
    return (
        <aside className="psy-graph-inspector">
            <div className="psy-graph-doc">
                <span>{doc.name}</span>
                <code>{doc.source_path}</code>
                <code>target: {doc.target_group}</code>
            </div>
            {node ? (
                <>
                    <h3>{node.title}</h3>
                    <code className="psy-graph-op">{node.op}</code>
                    <div className="psy-graph-values">
                        {Object.entries(node.values).map(([key, value]) => (
                            <ValueEditor
                                key={key}
                                label={key}
                                value={value}
                                on_change={(next) => on_change(node.id, key, next)}
                            />
                        ))}
                    </div>
                </>
            ) : (
                <div className="psy-empty">No node selected.</div>
            )}
            <div className="psy-graph-diagnostics">
                {doc.diagnostics.map((d, i) => (
                    <div key={`${d.level}-${i}`} className={`psy-diag is-${d.level}`}>
                        {d.text}
                    </div>
                ))}
            </div>
        </aside>
    );
}

function ValueEditor({
    label,
    value,
    on_change,
}: {
    label: string;
    value: PsyGraphValue;
    on_change(value: PsyGraphValue): void;
}) {
    if (Array.isArray(value)) {
        return (
            <label className="psy-graph-field">
                <span>{label}</span>
                <span className="psy-graph-vec3">
                    {value.map((component, i) => (
                        <input
                            key={i}
                            className="psy-input psy-input-number"
                            type="number"
                            step="0.01"
                            value={component}
                            onChange={(ev) => {
                                const next: [number, number, number] = [...value] as [number, number, number];
                                next[i] = Number(ev.target.value);
                                on_change(next);
                            }}
                        />
                    ))}
                </span>
            </label>
        );
    }
    if (typeof value === 'object') {
        if (value.type === 'linearIndex') {
            return (
                <label className="psy-graph-field">
                    <span>{label}</span>
                    <span className="psy-graph-pair">
                        <input
                            className="psy-input psy-input-number"
                            type="number"
                            step="0.01"
                            value={value.base}
                            onChange={(ev) => on_change({ ...value, base: Number(ev.target.value) })}
                        />
                        <input
                            className="psy-input psy-input-number"
                            type="number"
                            step="0.01"
                            value={value.step}
                            onChange={(ev) => on_change({ ...value, step: Number(ev.target.value) })}
                        />
                    </span>
                </label>
            );
        }
        return (
            <label className="psy-graph-field">
                <span>{label}</span>
                <input
                    className="psy-input psy-input-number"
                    type="number"
                    step="0.01"
                    value={value.value}
                    onChange={(ev) => on_change({ ...value, value: Number(ev.target.value) })}
                />
            </label>
        );
    }
    if (typeof value === 'number') {
        return (
            <label className="psy-graph-field">
                <span>{label}</span>
                <input
                    className="psy-input psy-input-number"
                    type="number"
                    step="0.01"
                    value={value}
                    onChange={(ev) => on_change(Number(ev.target.value))}
                />
            </label>
        );
    }
    return (
        <label className="psy-graph-field">
            <span>{label}</span>
            <input
                className="psy-input"
                type={typeof value === 'boolean' ? 'text' : 'text'}
                value={value_text(value)}
                onChange={(ev) => on_change(ev.target.value)}
            />
        </label>
    );
}

function serialize_graph(doc: PsyGraphDocument): string {
    const graph = {
        version: 1,
        name: doc.name,
        targetGroup: doc.target_group,
        nodes: doc.nodes
            .filter((node) => node.op !== 'on_update' && node.op !== 'compiled_behavior_op')
            .map((node) => ({
                id: node.id,
                op: node.op,
                ...node.values,
            })),
        links: doc.links,
    };
    return JSON.stringify(graph, null, 2);
}

function ExportDrawer({ json, on_close }: { json: string; on_close(): void }) {
    const [copied, set_copied] = React.useState(false);
    const copy = async () => {
        if (!navigator.clipboard) return;
        await navigator.clipboard.writeText(json);
        set_copied(true);
        setTimeout(() => set_copied(false), 1200);
    };
    return (
        <section className="psy-graph-export">
            <header>
                <span>Cookable PsyGraph JSON</span>
                <button type="button" className="psy-btn psy-btn-ghost" onClick={copy}>
                    {copied ? 'copied' : 'copy'}
                </button>
                <button type="button" className="psy-btn psy-btn-ghost" onClick={on_close}>
                    close
                </button>
            </header>
            <textarea className="psy-input" readOnly value={json} />
        </section>
    );
}
