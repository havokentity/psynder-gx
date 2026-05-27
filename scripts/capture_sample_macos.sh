#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Psynder-GX -- run a sample and capture its macOS window.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <sample-binary> [sample args...]" >&2
    exit 2
fi

SAMPLE=$1
shift

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
LOCK=/tmp/psynder_gx_smoke.lockdir
STAMP=$(date +%Y%m%d_%H%M%S)
OUT_DIR=${PSYNDER_CAPTURE_DIR:-/tmp/psynder_gx_capture_${STAMP}}
LOG_FILE="${OUT_DIR}/sample.log"

mkdir -p "$OUT_DIR"

while ! mkdir "$LOCK" 2>/dev/null; do
    sleep 0.5
done

PID=
cleanup() {
    if [[ -n "${PID}" ]] && kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rmdir "$LOCK" 2>/dev/null || true
}
trap cleanup EXIT

echo "[capture] output: $OUT_DIR"
echo "[capture] running: $SAMPLE $*"
"$SAMPLE" "$@" >"$LOG_FILE" 2>&1 &
PID=$!

# Give the app a moment to create its window, then ask macOS to surface it.
sleep "${PSYNDER_CAPTURE_WARMUP_SEC:-1.5}"
osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is ${PID}) to true" >/dev/null 2>&1 || true
sleep "${PSYNDER_CAPTURE_SETTLE_SEC:-0.5}"

OUT_PNG="${OUT_DIR}/window.png"
HELPER_SRC="${SCRIPT_DIR}/capture_window_macos.swift"
HELPER_BIN="${PSYNDER_CAPTURE_HELPER_BIN:-/tmp/psynder_gx_capture_window_macos}"
HELPER_CMD=()
if [[ -f "$HELPER_SRC" ]]; then
    if command -v xcrun >/dev/null 2>&1 &&
       [[ ! -x "$HELPER_BIN" || "$HELPER_SRC" -nt "$HELPER_BIN" ]]; then
        xcrun swiftc "$HELPER_SRC" -o "$HELPER_BIN" >/dev/null 2>&1 || true
    fi
    if [[ -x "$HELPER_BIN" ]]; then
        HELPER_CMD=("$HELPER_BIN")
    else
        HELPER_CMD=("$HELPER_SRC")
    fi
fi

CAPTURE_OK=0
if [[ ${#HELPER_CMD[@]} -gt 0 ]]; then
    START_SECONDS=$SECONDS
    TIMEOUT_SECONDS=${PSYNDER_CAPTURE_WINDOW_TIMEOUT_SEC:-8}
    while (( SECONDS - START_SECONDS < TIMEOUT_SECONDS )); do
        if "${HELPER_CMD[@]}" --pid "$PID" --out "$OUT_PNG"; then
            CAPTURE_OK=1
            break
        fi
        if ! kill -0 "$PID" 2>/dev/null; then
            break
        fi
        sleep 0.25
    done
fi

if [[ "$CAPTURE_OK" == "1" ]]; then
    file "$OUT_PNG" || true
else
    rm -f "$OUT_PNG"
    echo "[capture] warning: CoreGraphics window capture failed; trying System Events fallback" >&2

    # Capture just the sample window bounds. System Events reports global screen
    # coordinates, which can be negative on left/top secondary monitors.
    BOUNDS=$(
        osascript \
            -e "tell application \"System Events\" to tell (first process whose unix id is ${PID}) to get {position, size} of window 1" \
            2>/dev/null \
        | tr -d ' '
    ) || BOUNDS=

    if [[ -n "$BOUNDS" ]]; then
        IFS=',' read -r X Y W H <<<"$BOUNDS"
        echo "[capture] window bounds: x=${X} y=${Y} w=${W} h=${H}"
        screencapture -x -R"${X},${Y},${W},${H}" "$OUT_PNG" || true
        if [[ -s "$OUT_PNG" ]]; then
            file "$OUT_PNG" || true
        else
            rm -f "$OUT_PNG"
            echo "[capture] warning: window capture failed" >&2
        fi
    else
        echo "[capture] warning: could not query sample window bounds" >&2
    fi
fi

if [[ "${PSYNDER_CAPTURE_ALL_DISPLAYS:-0}" == "1" ]]; then
    for display in 1 2 3 4 5 6 7 8; do
        out="${OUT_DIR}/display_${display}.png"
        if screencapture -x -D"${display}" "$out" >/dev/null 2>&1 && [[ -s "$out" ]]; then
            file "$out" || true
        else
            rm -f "$out"
        fi
    done
fi

set +e
wait "$PID"
STATUS=$?
set -e
PID=

cat "$LOG_FILE"
echo "[capture] sample exit: $STATUS"
echo "[capture] output: $OUT_DIR"
exit "$STATUS"
