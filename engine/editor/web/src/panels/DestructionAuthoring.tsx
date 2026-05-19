// SPDX-License-Identifier: MIT
// Psynder-GX — Destruction Authoring panel stub.
//
// GX-specific: pre-fracture chunk editing + joint strength tuning (DESIGN §10.8, M7).
// Full implementation deferred to M7.
//
// TODO(M7): Wire to IPC channel "destruction_author" (msgpack) to drive
//           lane/17-physics-destruction chunk pre-fracture and
//           structural-integrity graph parameters.
// TODO(M7): Add chunk hierarchy tree view, joint strength heat-map overlay,
//           cascade-sim trigger buttons, debris pool size control.

import React from 'react';

export const DestructionAuthoring: React.FC = () => (
    <div style={{ fontFamily: 'system-ui', padding: 16 }}>
        <h2>Destruction Authoring</h2>
        <p style={{ color: '#888' }}>
            [stub — M7] Pre-fracture chunk editor: Voronoi/MLSPH slice controls,
            joint strength tuning, structural-integrity graph, cascade-sim preview.
        </p>
    </div>
);

export default DestructionAuthoring;
