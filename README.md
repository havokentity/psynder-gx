# Psynder-GX

> *Best GPU opts. Best CPU opts. FPS-centric and focused. Latest rendering quality. **HIGHLY OPTIMIZED for HIGH FPS.***

**Psynder-GX** is the **GPU-accelerated competitive FPS engine** in the Psynder family. The goal is a *lightweight, open-source engine you'd genuinely pick over Unity or Unreal* for a serious indie or pro-indie competitive shooter — not because it does more, but because it does less, faster, and with cleaner architecture.

Sister project: [Psynder](https://github.com/havokentity/psynder) — the pure-CPU software-rendering engine in the same family. Most foundation code (math, ECS, jobs, audio, scripting, editor, platform) is shared.

**Status:** v1.0 design handoff (2026-05-19), bootstrap in progress via [25-agent parallel build](PLAN.md).

## Design at a glance

- **Targets:** Windows x86-64, Linux x86-64, macOS Apple Silicon — all first-class.
- **Graphics APIs:** **Vulkan** on Windows + Linux. **Native Metal** on Apple Silicon. No D3D12. No MoltenVK.
- **Language:** C++23 (Clang ≥ 17 primary).
- **Architecture:** GPU-driven rendering, forward+ shading, hardware ray-traced shadows + DDGI where available, hybrid SSR+RT reflections.
- **Physics:** Jolt + custom destruction (chunk-based) + custom vehicles (Pacejka tires, raycast suspension, drivetrain). Real metric units throughout.
- **Netcode:** Server-authoritative, 128-tick capable, lag compensation with rewind hit-tracing, `.psydem` demo recording, voice chat (Opus, server-mixed, positional).
- **Map scale:** 2 km² (~1.4 km × 1.4 km), 64-player cap.
- **License:** MIT OR Apache-2.0 (dual). Assets under CC-BY-4.0.

For the full design, see [DESIGN-PSYNDER-GX.md](DESIGN-PSYNDER-GX.md) (916 lines, 16 ADRs all decided).

## Reference games

CS:GO + CS2 + Battlefield 3/4 + Squad/Insurgency + Hell Let Loose. We're chasing competitive-FPS feel at indie scale.

## Repository layout

```
psynder-gx/
├── DESIGN-PSYNDER-GX.md     ← canonical design doc, v1.0
├── PLAN.md                  ← 25-agent parallel build plan
├── AGENTS.md                ← lane ownership + coordination rules
├── engine/                  ← 25 lanes (see PLAN.md §2 for the table)
│   ├── core/  math/  simd/  jobs/  asset/  scene/
│   ├── gpu/  shader/        ← Vulkan + Metal abstraction; Slang shader pipeline
│   ├── render/{pipeline,rt,post}/
│   ├── world/{bsp,outdoor}/
│   ├── audio/  physics/{core,vehicle,destruction}/
│   ├── net/{*, voice/}      ← rUDP + lag comp + Opus voice
│   ├── script/  ui/
│   ├── editor/{core,ipc,web}/
│   └── platform/{win32,linux,macos}/
├── samples/                 ← 00_clear → 07_bflight_map demos
├── tools/                   ← lm_cook_gx, lm_bake_gx, lm_qbsp, lm_pak
├── tests/                   ← unit + golden-image + netcode determinism + bench
├── third_party/             ← Jolt, Opus, FreeType, RmlUi, Slang, vulkan-cpp/metal-cpp
├── cmake/                   ← shared CMake helpers (CompilerWarnings, Lanes, Deps, etc.)
└── .github/workflows/       ← Win + Linux + Linux-dedicated + macOS CI
```

## Building

Prereqs:
- CMake ≥ 3.28, Ninja, a recent Clang (≥ 17) or MSVC 19.40+
- **Vulkan SDK** (LunarG) on Windows + Linux. **Xcode + Metal SDK** on macOS.
- **Node.js ≥ 20 + pnpm** for editor panels (skip if building dedicated-server only).
- vcpkg (manifest mode, fetched on first configure).

```bash
# Mac (Apple Silicon)
cmake --preset mac-release && cmake --build --preset mac-release

# Linux
cmake --preset linux-release && cmake --build --preset linux-release

# Windows (clang-cl)
cmake --preset win-clang-release && cmake --build --preset win-clang-release

# Linux dedicated server (headless, no Vulkan / audio / editor)
cmake --preset linux-dedicated-release && cmake --build --preset linux-dedicated-release
```

Smoke test (M0 — animated clear color):

```bash
build/<preset>/bin/sample_00_clear --smoke-frames=120
```

## Contributing

This repo is built in parallel by 25 lane agents working in worktrees off `main`. Each lane owns one directory. **Read [AGENTS.md](AGENTS.md) before touching anything** — file ownership is strict and cross-lane edits go via Issues against the orchestrator.

If you're a human contributor: jump to the lane that matches your interest (rendering, networking, physics-destruction, editor, etc.) and open a PR against `main`. Squash-merge, ADRs for cross-system changes, Contributor Covenant 2.1.

## License

Engine code: **MIT OR Apache-2.0** at your choice (`LICENSE-MIT`, `LICENSE-APACHE`). See [NOTICE](NOTICE) for vendored dependency attributions.

Sample assets (under `samples/*/assets/`): CC-BY-4.0.

## Acknowledgements

- Author's sister project **Psynder** (CPU engine) — `engine/*` foundation modules (math, jobs, ECS, audio, scripting, editor, platform) are vendored from there.
- Author's sister project **DeMonT PathTracer** — console + early hardware-detection scaffolds.
- **Jolt Physics** (MIT, vendored) for the physics core.
- **Opus** (BSD, vendored) for voice chat.
- **Slang** (MIT-ish, vendored) for shader compilation.
- **RmlUi** (MIT, vendored) for player HUDs.
- **FreeType** (FTL, vendored) for font rasterization.
- **NVIDIA / AMD / Intel / Apple** vendor upscaler SDKs (DLSS / FSR / XeSS / MetalFX) are SDK-licensed and integrated as thin wrappers.

---

*Engine you'd actually pick for a serious indie competitive shooter.*
