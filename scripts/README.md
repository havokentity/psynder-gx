# scripts/

Developer helpers. Most CI / build wiring lives in `cmake/` and
`.github/workflows/`; this directory is for ad-hoc dev scripts.

- `build_release.sh` — builds the editor web bundle and release targets from a
  terminal. Defaults to the host release preset (`mac-release`,
  `linux-release`, or `win-release`); override with `PSYNDER_GX_PRESET=...`.
- `run_release.sh` — runs `build/<preset>/bin/PsyArcadeGX` from the repo root so
  project-relative assets resolve. Passes through any extra command-line args.
- `smoke_sample.sh` — acquires the `/tmp/psynder_gx_smoke.lockdir` mutex and
  runs a sample binary headless for N frames (Mac parallel-agent safety).
- `capture_sample_macos.sh` — runs a sample under the same mutex and captures
  its macOS window to `/tmp/.../window.png`.
- `capture_window_macos.swift` — CoreGraphics PID/window-id capture helper used
  by `capture_sample_macos.sh`; prints exact window bounds and avoids fragile
  AppleScript accessibility window lookup on multi-monitor setups.
- Per-lane fixture scripts may land here as lanes file PRs.
