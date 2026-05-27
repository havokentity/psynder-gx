/// <reference types="node" />

import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

const parsed_engine_ipc_port = Number(process.env.PSYNDER_GX_EDITOR_IPC_PORT ?? '7655');
const ENGINE_IPC_PORT =
    Number.isInteger(parsed_engine_ipc_port) &&
    parsed_engine_ipc_port > 0 &&
    parsed_engine_ipc_port <= 65535
        ? parsed_engine_ipc_port
        : 7655;

export default defineConfig({
    base: './',
    plugins: [react()],
    server: {
        port: 5173,
        proxy: {
            '/api': `http://127.0.0.1:${ENGINE_IPC_PORT}`,
            '/ws':  { target: `ws://127.0.0.1:${ENGINE_IPC_PORT}`, ws: true },
        },
    },
    build: {
        outDir: 'dist',
        sourcemap: true,
    },
});
