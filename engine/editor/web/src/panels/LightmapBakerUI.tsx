// SPDX-License-Identifier: MIT
// Psynder-GX — Lightmap / Probe Baker UI panel stub.
//
// GX-specific: trigger lm_bake_gx runs + probe density visualisation (DESIGN §10.8, M5).
// Full implementation deferred to M5.
//
// TODO(M5): Wire to IPC channel "lm_baker" (msgpack) to dispatch bake jobs
//           to the offline lm_bake_gx tool (PLAN.md §0, tools/lm_bake_gx/).
// TODO(M5): Add texel density heatmap overlay, DDGI probe grid density viz,
//           per-surface UV unwrap quality inspector, bake progress log.

import React from 'react';

export const LightmapBakerUI: React.FC = () => (
    <div style={{ fontFamily: 'system-ui', padding: 16 }}>
        <h2>Lightmap / Probe Baker</h2>
        <p style={{ color: '#888' }}>
            [stub — M5] Trigger lm_bake_gx offline bake jobs, monitor progress,
            inspect texel density, visualise DDGI probe grid placement.
        </p>
    </div>
);

export default LightmapBakerUI;
