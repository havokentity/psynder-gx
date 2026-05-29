#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Perf / DOTS / determinism guardrails — wired as a ctest (`perf_guardrails`) so
# every autonomous iteration AND CI fails loud on a regression rather than
# letting hot-path discipline rot. See docs/AUTONOMOUS-CHARTER.md §1.
#
# What it enforces (mechanical, low-false-positive — the rest is the per-iteration
# self-audit in the charter):
#   1. DOTS hot-path discipline: no std::shared_ptr / make_shared / RTTI
#      (dynamic_cast / typeid) / exceptions (throw) in the determinism-critical
#      fixed-tick SoA lanes. (DESIGN §3, AGENTS.md.)
#   2. Determinism FP flags applied to every lockstep-sensitive lane's CMakeLists
#      (the hole that previously let the physics lane miss MSVC /fp:strict).

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0

# ── 1. Hot-path discipline ──────────────────────────────────────────────────
HOT_PATHS=(
  engine/physics/agents
  engine/physics/destruction
  engine/physics/core/EcsCharacterBridge.h
  engine/net/ReplicationSession.cpp engine/net/ReplicationSession.h
  engine/net/SnapshotReplication.h engine/net/Prediction.h
  engine/net/SnapshotDelta.cpp engine/net/SnapshotDelta.h
  engine/net/LagComp.cpp engine/net/LagComp.h
  engine/net/TickConfig.h engine/net/InterestManagement.h
)
# Auto-cover future DOTS gameplay/AI lanes the moment they exist.
for d in engine/gameplay engine/ai engine/physics/weapons; do
  [ -e "$ROOT/$d" ] && HOT_PATHS+=("$d")
done

PAT='std::shared_ptr|std::make_shared|dynamic_cast|\btypeid\b|\bthrow\b'
for p in "${HOT_PATHS[@]}"; do
  [ -e "$ROOT/$p" ] || continue
  # Strip whole-line comments so doc text mentioning these words doesn't trip it.
  hits="$(grep -rnE "$PAT" "$ROOT/$p" 2>/dev/null \
          | grep -vE ':[0-9]+:[[:space:]]*(//|\*|/\*|///)' || true)"
  if [ -n "$hits" ]; then
    echo "PERF GUARDRAIL FAIL: forbidden DOTS-hot-path construct under $p"
    echo "  (no shared_ptr / RTTI / exceptions in the fixed-tick lockstep path)"
    echo "$hits"
    fail=1
  fi
done

# ── 2. Determinism FP flags on lockstep-sensitive lanes ─────────────────────
for lane in physics/core physics/agents physics/destruction net; do
  cm="$ROOT/engine/$lane/CMakeLists.txt"
  [ -f "$cm" ] || continue
  if ! grep -qE 'psynder_determinism_fp|psynder_hot_lane|fno-fast-math|/fp:strict' "$cm"; then
    echo "PERF GUARDRAIL FAIL: engine/$lane missing determinism FP flags"
    echo "  (call psynder_determinism_fp(<target>) in its CMakeLists)"
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "perf_guardrails: FAIL"
  exit 1
fi
echo "perf_guardrails: OK (DOTS hot-path discipline + determinism flags intact)"
exit 0
