// SPDX-License-Identifier: MIT
// Psynder-GX — Material Editor panel stub.
//
// GX-specific: Slang-based material authoring (DESIGN §10.8).
// Full implementation deferred to M2–M3.
//
// TODO(M2): Wire to IPC channel "material_editor" (msgpack) for live
//           parameter binding to the Slang shader graph.
// TODO(M3): Add node graph UI, texture slot drag-drop, BRDF preview sphere.

import React from 'react';

export const MaterialEditor: React.FC = () => (
    <div style={{ fontFamily: 'system-ui', padding: 16 }}>
        <h2>Material Editor</h2>
        <p style={{ color: '#888' }}>
            [stub — M2/M3] Slang material authoring: node graph, BRDF parameters,
            texture slot binding, live shader compile &amp; preview.
        </p>
    </div>
);

export default MaterialEditor;
