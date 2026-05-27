#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run the release PsyArcadeGX player/editor target from the repo root.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

host_preset() {
    case "$(uname -s)" in
        Darwin) echo "mac-release" ;;
        Linux)  echo "linux-release" ;;
        MINGW*|MSYS*|CYGWIN*) echo "win-release" ;;
        *) echo "mac-release" ;;
    esac
}

PRESET="${PSYNDER_GX_PRESET:-$(host_preset)}"
APP="${PSYNDER_GX_APP:-$ROOT/build/$PRESET/bin/PsyArcadeGX}"

if [[ ! -x "$APP" ]]; then
    echo "[run-release] missing executable: $APP" >&2
    echo "[run-release] build it first: scripts/build_release.sh" >&2
    exit 1
fi

echo "[run-release] preset: $PRESET"
echo "[run-release] app: $APP"
echo "[run-release] args: $*"

cd "$ROOT"
exec "$APP" "$@"
