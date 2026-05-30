# Resume the Autonomous Multi-Agent Loop

How to restart the self-driving Psynder-GX build in a **fresh Claude Code session**.
Read this with `docs/AUTONOMOUS-CHARTER.md` (rules) and `docs/AUTONOMOUS-ROADMAP.md`
(living backlog + journal). The loop runs **4-agent parallel batches** by default.

---

## TL;DR — one paste to resume

Open Claude Code **in this repo** (`/Volumes/XTRM 5 Media/More MyRepos/Psynder-GPU`)
and paste the **SETUP PROMPT** below. It will (a) create the recurring cron driver
that fires the loop every 30 min, and (b) immediately run the first batch.

If you only want a single batch right now (no cron), paste the **DRIVER PROMPT**
instead — it runs exactly one iteration and stops.

---

## SETUP PROMPT  (creates the cron + runs iteration 1)

```
Resume the autonomous Psynder-GX engine loop in 4-agent-batch mode.

First, create a recurring cron job (CronCreate, cron "7,37 * * * *", recurring=true)
whose prompt is the DRIVER PROMPT in docs/RESUME-AUTONOMOUS.md (copy it verbatim).
Then immediately execute one iteration of that DRIVER PROMPT yourself now — don't
wait for the first cron fire. Keep going autonomously; never stop for input.
```

> The cron is **session-only** (it stops when you close the session). To stop it
> any time: ask "delete the autoloop cron" (CronDelete) or just close the session.

---

## DRIVER PROMPT  (what each iteration runs — also the cron's prompt)

```
Autonomous Psynder-GX engine loop — durable resume driver, 4-AGENT BATCH MODE.

STEP 0 (single-runner lock): run `mkdir /tmp/psynder_gx_autoloop.lockdir`. If it
FAILS (already held), another iteration is in flight — do nothing and end this
fire; do NOT remove the lock. If it succeeds, ensure you
`rmdir /tmp/psynder_gx_autoloop.lockdir` when this iteration finishes (success or
failure).

Then: cd "/Volumes/XTRM 5 Media/More MyRepos/Psynder-GPU"; read
docs/AUTONOMOUS-CHARTER.md + docs/AUTONOMOUS-ROADMAP.md; verify the previous push's
CI (ci + determinism on nextgen/new-release) is GREEN and fix it before anything
else.

Then execute ONE iteration as a 4-AGENT PARALLEL BATCH (Charter §3 + §5):
  1. Pick up to 4 DISJOINT, unblocked roadmap items — each in a DISTINCT lane dir,
     each ADDITIVE (new files) or strictly BACKWARD-COMPATIBLE (opt-in params).
     Avoid the golden-digest / determinism-critical hot paths (engine/physics/
     agents steering, engine/math, engine/scene core) for parallel agents — do
     those SOLO.
  2. Spawn the agents concurrently (Agent tool, subagent_type general-purpose).
     Give each a HARDENED prompt: implement ONLY its listed files in its ONE lane;
     do NOT run cmake/ninja/ctest/git; do NOT touch build/ or any shared file (top
     CMakeLists.txt, tests/unit/CMakeLists.txt) or other lanes; study neighbor
     files for conventions (SPDX header, namespace, strict-FP determinism, metric
     units, no shared_ptr/dynamic_cast/throw in hot lanes); write compile-ready
     C++23 + a Catch2 test. A lane MAY edit its OWN engine/<lane>/CMakeLists.txt.
  3. SERIALLY INTEGRATE (you, not the agents): `git status` to confirm only the
     expected disjoint files changed (no shared-file edits); reconfigure + build
     mac-debug; fix any compile errors (incremental builds are fast); run the new
     test groups; then full ctest GREEN incl perf_guardrails under the smoke lock
     + a headless smoke (./build/mac-debug/bin/PsyServerGX --ticks=128 and
     sample_02_crate --smoke-frames=20); then build mac-release + run the
     determinism subset (ctest -R "golden|determinism|deterministic|replay|
     lockstep") to confirm the golden pin holds.
  4. Commit (NO Co-Authored-By trailer, imperative subject <70 chars, ONE commit
     for the batch) + push to nextgen/new-release; WATCH CI to green (poll the ci
     + determinism workflows); append a journal entry to AUTONOMOUS-ROADMAP.md;
     release the lock (`rmdir /tmp/psynder_gx_autoloop.lockdir`).

If <3 good disjoint items exist, do a smaller batch or a SOLO iteration instead.
Honor every pillar + the §1b perf/DOTS/SIMD guardrails; leave the tree green +
pushed. Never stop for input; press through blockers per §4 (pivot or spin up a
demo rather than grind). Stop only at the Definition of Done (§8).

Smoke-lock pattern (serialize runtime/sample invocations):
  LOCK=/tmp/psynder_gx_smoke.lockdir; while ! mkdir "$LOCK" 2>/dev/null; do sleep 0.5; done; trap 'rmdir "$LOCK"' EXIT
Build: cmake --preset mac-debug && cmake --build --preset mac-debug
Test:  (cd build/mac-debug && ctest)
```

---

## Current state (as of last session — commit `d2fff60`)

- **Branch:** `nextgen/new-release` — green on CI (full macOS+Linux+Windows matrix
  + determinism). 715 unit tests passing.
- **Locks:** both free. **Cron:** none scheduled (delete/recreate as needed).
- **Built this run (iters 10–25):** cross-platform FP-determinism gate; full
  netcode→gameplay→match→dedicated-server stack; Quake3 movement; broadphase
  combat AI + A* + path-simplify; outdoor terrain (query/clamp/slope/spawns);
  graph→IR→ECS PsyGraph scripting; weapon spread/ballistics; snapshot quantize/
  pack. Three clean 4-agent batches landed 12 features.
- **Frontier (mostly in-window — needs human verification):** GPU terrain/BSP
  draw, `render::pipeline` adoption + PBR, the WYSIWYG editor + PsyGraph node UI,
  live IPC. Headless items still open: UDP transport binding, IR SIMD back-end,
  wiring batches (Ballistics→fire_hitscan, SnapshotPack→wire codec, A*→a bot,
  terrain_walkable→spawn validation).

---

## Safety invariants (why this is restart-safe)

- **Single-runner lock** (`/tmp/psynder_gx_autoloop.lockdir`) — only one iteration
  runs at a time; a second cron fire mid-iteration no-ops.
- **Smoke lock** (`/tmp/psynder_gx_smoke.lockdir`) — serializes Mac binary/sample
  runs (window/Metal contention). If a crash leaves it held, `rmdir` it.
- **Disjoint-lane + additive discipline** — parallel agents never touch shared
  files or each other's lanes, so batches merge without conflict. The serial
  integration (build + full ctest + determinism + CI) is the safety gate.
- **Durable state** lives in git (`nextgen/new-release`) + AUTONOMOUS-ROADMAP.md;
  a fresh session reconstructs everything from those.

## If something is red on resume

The DRIVER PROMPT verifies + fixes CI **before** new work. If CI is red, it fixes
that first. If a lock is stuck from a crashed session: `rmdir
/tmp/psynder_gx_autoloop.lockdir /tmp/psynder_gx_smoke.lockdir` then re-run.
