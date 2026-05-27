#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Psynder — smoke-run a sample binary under the shared Mac smoke lock.
# Acquires /tmp/psynder_gx_smoke.lockdir atomically (mkdir is POSIX-atomic),
# then runs the sample binary. Releases the lock on shell exit via trap.
# If a previous app was killed hard, stale owner metadata is used to self-heal
# the lock instead of leaving every future smoke blocked.

set -euo pipefail

LOCK=/tmp/psynder_gx_smoke.lockdir
LOCK_PID="$LOCK/pid"
LOCK_INFO="$LOCK/info"
STALE_SECONDS=${PSYNDER_SMOKE_LOCK_STALE_SECONDS:-60}
SAMPLE=${1:-build/mac-release/bin/sample_00_clear}
ARGS=("${@:2}")

lock_mtime() {
    stat -f %m "$LOCK" 2>/dev/null || stat -c %Y "$LOCK" 2>/dev/null || echo 0
}

try_reap_stale_lock() {
    if [[ -f "$LOCK_PID" ]]; then
        local owner
        owner=$(cat "$LOCK_PID" 2>/dev/null || true)
        if [[ "$owner" =~ ^[0-9]+$ ]] && kill -0 "$owner" 2>/dev/null; then
            return
        fi
        echo "[smoke] $(date -Iseconds) removing stale smoke lock from pid ${owner:-unknown}"
        rm -f "$LOCK_PID" "$LOCK_INFO" 2>/dev/null || true
        rmdir "$LOCK" 2>/dev/null || true
        return
    fi

    local now mtime age
    now=$(date +%s)
    mtime=$(lock_mtime)
    age=$((now - mtime))
    if (( age >= STALE_SECONDS )); then
        echo "[smoke] $(date -Iseconds) removing legacy stale smoke lock age=${age}s"
        rmdir "$LOCK" 2>/dev/null || true
    fi
}

while ! mkdir "$LOCK" 2>/dev/null; do
    try_reap_stale_lock
    sleep 0.5
done
printf '%s\n' "$$" > "$LOCK_PID"
printf 'pid=%s\nsample=%s\nstarted=%s\n' "$$" "$SAMPLE" "$(date -Iseconds)" > "$LOCK_INFO"
trap 'rm -f "$LOCK_PID" "$LOCK_INFO"; rmdir "$LOCK"' EXIT

if ((${#ARGS[@]})); then
    echo "[smoke] $(date -Iseconds) running $SAMPLE ${ARGS[*]}"
    "$SAMPLE" "${ARGS[@]}"
else
    echo "[smoke] $(date -Iseconds) running $SAMPLE"
    "$SAMPLE"
fi
echo "[smoke] $(date -Iseconds) done"
