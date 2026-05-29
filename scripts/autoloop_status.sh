#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Autonomous-run status / resume helper — the "where is it / am I back?" tool.
#
# Prints how far the 12h autonomous engine build has gotten: elapsed time, open
# backlog, recent commits, latest CI, the single-runner lock state, and the last
# journal entry. Run it any time to check progress without disturbing the loop.
#
# RESUME after a full restart (the loop's drivers are session-only here): just
# start Claude Code in this repo and say "continue the autonomous run" (or re-run
# the /loop command). All state lives in docs/AUTONOMOUS-ROADMAP.md + git, so a
# fresh session reconstructs context losslessly from the charter + roadmap.

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== Psynder-GX autonomous run status =="
date '+now:     %Y-%m-%d %H:%M %Z'
grep -m1 'Started:' docs/AUTONOMOUS-ROADMAP.md 2>/dev/null | sed 's/^- //'
echo
echo "-- open backlog (next unchecked items) --"
grep -nE '^\s*- \[ \]' docs/AUTONOMOUS-ROADMAP.md 2>/dev/null | head -24
echo
echo "-- recent commits (nextgen/new-release) --"
git log --oneline -10 nextgen/new-release 2>/dev/null
echo
echo "-- latest CI --"
gh run list --branch nextgen/new-release --limit 3 2>/dev/null || echo "(gh unavailable)"
echo
echo "-- single-runner lock --"
if [ -d /tmp/psynder_gx_autoloop.lockdir ]; then
  echo "HELD — an iteration is in flight"
else
  echo "free — no iteration running"
fi
echo
echo "-- latest journal --"
awk '/^## Journal/{j=1} j' docs/AUTONOMOUS-ROADMAP.md 2>/dev/null | tail -14
