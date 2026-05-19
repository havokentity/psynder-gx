// SPDX-License-Identifier: MIT
// Psynder-GX — Replay Viewer panel stub.
//
// GX-specific: .psydem demo file playback (DESIGN §10.8, M6).
// Full implementation deferred to M6.
//
// TODO(M6): Wire to IPC channel "replay" (msgpack) to drive
//           lane/18-net .psydem snapshot replay and scrubbing.
// TODO(M6): Add timeline scrubber, tick-accurate playback controls,
//           player POV switcher, network delta inspector.

import React from 'react';

export const ReplayViewer: React.FC = () => (
    <div style={{ fontFamily: 'system-ui', padding: 16 }}>
        <h2>Replay Viewer</h2>
        <p style={{ color: '#888' }}>
            [stub — M6] .psydem demo playback: timeline scrubber, tick-accurate
            controls, player POV switcher, network delta inspection.
        </p>
    </div>
);

export default ReplayViewer;
