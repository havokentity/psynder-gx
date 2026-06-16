<!-- SPDX-License-Identifier: MIT OR Apache-2.0 -->
# Third-party dependency versions (SBOM)

> Canonical list of the third-party code Psynder-GX pulls in, **how** each piece
> is acquired, and where its version is pinned. `NOTICE` and `README.md` point
> here for per-release version info.
>
> Last reconciled against the tree: **2026-05-31**.

## A note on "vendored"

`README.md` and `NOTICE` describe most dependencies as *vendored* under
`third_party/<name>/`. That wording is historical and **only partly true today**.
The current build acquires the large dependencies through **CMake FetchContent**
(pinned by tag/SHA) or **vcpkg** (the `vcpkg.json` manifest) — they are *not*
committed under `third_party/`. What is actually checked in under `third_party/`
is: `stb/`, `fonts/` (JetBrains Mono), and the sister-project reference snapshots
(`pathtracer-refs/`, `psynder-refs/`). Treat the table below — not the
`third_party/<name>/` paths in `NOTICE` — as the source of truth for what the
build pulls.

## Acquired via FetchContent (pinned)

| Dependency | Version / pin | License | Pin location |
|---|---|---|---|
| Jolt Physics | **v5.5.0** — SHA `23dadd0e603f1b321142d4c74df07fce85064989` | MIT | `engine/physics/core/CMakeLists.txt` |
| Opus | **v1.5.2** (git tag) | BSD-3-Clause + RFC 6716 IPR | `engine/net/voice/CMakeLists.txt` |
| Lua | **5.4.7** (lua.org source tarball) | MIT | `engine/script/CMakeLists.txt` |

Jolt is deliberately pinned by **commit SHA, not tag**, because lockstep replay
is bitwise-deterministic and a moved/re-cut tag would silently change physics
behaviour (the comment in the pin location documents the verification). Opus and
Lua are pinned by immutable release tag/tarball.

## Acquired via vcpkg (`vcpkg.json` manifest)

> ⚠️ **No `builtin-baseline` is set** in `vcpkg.json`, so these versions **float**
> to whatever the active vcpkg registry resolves at configure time. This is a
> known reproducibility gap (ARCH review B3 — same class of issue as pinning
> Jolt by SHA). Pin a baseline before tagging a release.

| Dependency | Acquisition | License |
|---|---|---|
| fmt | vcpkg core dep | MIT |
| zstd | vcpkg core dep | BSD-3-Clause |
| freetype | vcpkg core dep | FTL or GPL-2.0 (we use FTL) |
| vulkan-headers, vulkan-loader, vulkan-memory-allocator, volk | vcpkg core deps | Apache-2.0 / MIT |
| spirv-cross, spirv-headers, glslang, shaderc | vcpkg core deps (slangc fallback toolchain) | Apache-2.0 / MIT |
| tinygltf, stb, meshoptimizer, ktx | vcpkg core deps (`stb` also committed under `third_party/stb/`) | MIT / public-domain |
| catch2 | vcpkg `tests` feature | BSL-1.0 |
| tracy | vcpkg `tracy` feature | BSD-3-Clause |
| embree | vcpkg `tools` feature | Apache-2.0 |
| rmlui | vcpkg `rmlui` feature | MIT |

## System / SDK tools (not bundled)

| Tool | Version | Discovery | License |
|---|---|---|---|
| Slang (`slangc`) | **2026.1** | `find_program` → baked into the build as `PSYNDER_SLANGC_PATH` (`engine/shader/CMakeLists.txt`); used by the runtime shader compiler. Falls back to `glslc` + `spirv-cross` when absent. | MIT-ish (see project LICENSE) |
| metal-cpp | Apple SDK | bundled header bindings (macOS only) | Apache-2.0 |
| NVIDIA DLSS / AMD FSR / Intel XeSS / Apple MetalFX | per-SDK | optional, present only when the SDK is installed locally | separate licenses |

## Actually committed under `third_party/`

| Path | What | License |
|---|---|---|
| `third_party/stb/` | stb single-file libraries | public domain or MIT |
| `third_party/fonts/JetBrainsMono/` | JetBrains Mono font family | SIL OFL 1.1 |
| `third_party/pathtracer-refs/` | reference snapshots from the dmonte PathTracer project | MIT |
| `third_party/psynder-refs/` | reference snapshots from the sister Psynder (CPU engine) project | MIT |

Sister-project engine modules (`engine/core`, `math`, `simd`, `jobs`, `scene`,
`asset`, `audio`, …) are derived from Psynder and are first-party code in this
repo under MIT OR Apache-2.0; see `NOTICE` for the attribution.

## Maintenance

When bumping any pin: update the version here **and** in the pin location, then
re-run the golden / determinism suite (physics + netcode are determinism-critical
— a Jolt or FP-behaviour change can break lockstep replay). When adding a vcpkg
baseline, record the baseline commit in this file too.
