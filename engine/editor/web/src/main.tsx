// SPDX-License-Identifier: MIT
// Psynder-GX editor web entrypoint.

import React from 'react';
import { createRoot } from 'react-dom/client';

import { App } from './App';
import { get_client } from './ipc/client';
import './styles/panels.css';

get_client();

const container = document.getElementById('root');
if (!container) throw new Error('Psynder-GX editor: missing #root element');

createRoot(container).render(
    <React.StrictMode>
        <App />
    </React.StrictMode>,
);
