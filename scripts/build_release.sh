#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Psynder-GX release targets from a terminal.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WEB_DIR="$ROOT/engine/editor/web"

host_preset() {
    case "$(uname -s)" in
        Darwin) echo "mac-release" ;;
        Linux)  echo "linux-release" ;;
        MINGW*|MSYS*|CYGWIN*) echo "win-release" ;;
        *) echo "mac-release" ;;
    esac
}

PRESET="${PSYNDER_GX_PRESET:-$(host_preset)}"
BUILD_WEB="${PSYNDER_GX_BUILD_WEB:-1}"

if (($#)); then
    TARGETS=("$@")
else
    TARGETS=(PsyArcadeGX sample_02_crate psynder_editor_scene_authoring)
fi

echo "[build-release] root: $ROOT"
echo "[build-release] preset: $PRESET"
echo "[build-release] targets: ${TARGETS[*]}"

if [[ "$BUILD_WEB" != "0" && -f "$WEB_DIR/package.json" ]]; then
    if [[ ! -d "$WEB_DIR/node_modules" ]]; then
        echo "[build-release] installing web dependencies"
        npm --prefix "$WEB_DIR" ci
    fi
    echo "[build-release] building editor web"
    npm --prefix "$WEB_DIR" run build
fi

cmake --build --preset "$PRESET" --target "${TARGETS[@]}"

echo "[build-release] done"
echo "[build-release] app: $ROOT/build/$PRESET/bin/PsyArcadeGX"
