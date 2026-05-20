# Psynder-GX — agent coordination

When multiple agents work on Psynder-GX in parallel (the standard mode of operation — see [PLAN.md](PLAN.md) and the 25-lane carve-up), strict ownership prevents merge hell. This file is the load-bearing reference.

## File ownership per lane

Every parallel agent owns ONE directory plus its subdirectory `CMakeLists.txt`. **The agent never touches files outside its owned set.** Cross-cutting edits (top-level `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, root `cmake/*.cmake`, `.github/`, `.clang-*`, `LICENSE-*`, `README.md`, `DESIGN-PSYNDER-GX.md`, `AGENTS.md`, `PLAN.md`) are reserved for the orchestrator and are not in any lane.

If a lane agent finds it needs a file outside its ownership, **STOP** and document in the PR body. The orchestrator mediates.

## Lane → directory map

| Lane | Directory | Branch | Wave |
|---|---|---|---|
| 01-core               | `engine/core/`                | `lane/01-core`                | A |
| 02-math               | `engine/math/`                | `lane/02-math`                | A |
| 03-simd               | `engine/simd/`                | `lane/03-simd`                | A |
| 04-jobs               | `engine/jobs/`                | `lane/04-jobs`                | A |
| 05-asset              | `engine/asset/`               | `lane/05-asset`               | A |
| 06-scene              | `engine/scene/`               | `lane/06-scene`               | A |
| 07-gpu                | `engine/gpu/`                 | `lane/07-gpu`                 | B |
| 08-shader             | `engine/shader/`              | `lane/08-shader`              | B |
| 09-render-pipeline    | `engine/render/pipeline/`     | `lane/09-render-pipeline`     | B |
| 10-render-rt          | `engine/render/rt/`           | `lane/10-render-rt`           | B |
| 11-render-post        | `engine/render/post/`         | `lane/11-render-post`         | B |
| 12-world-bsp          | `engine/world/bsp/`           | `lane/12-world-bsp`           | A |
| 13-world-outdoor      | `engine/world/outdoor/`       | `lane/13-world-outdoor`       | A |
| 14-audio              | `engine/audio/`               | `lane/14-audio`               | B |
| 15-physics-core       | `engine/physics/core/`        | `lane/15-physics-core`        | B |
| 16-physics-vehicle    | `engine/physics/vehicle/`     | `lane/16-physics-vehicle`     | B |
| 17-physics-destruction| `engine/physics/destruction/` | `lane/17-physics-destruction` | B |
| 18-net                | `engine/net/` (excl. voice/)  | `lane/18-net`                 | B |
| 19-net-voice          | `engine/net/voice/`           | `lane/19-net-voice`           | B |
| 20-script             | `engine/script/`              | `lane/20-script`              | B |
| 21-ui                 | `engine/ui/`                  | `lane/21-ui`                  | B |
| 22-editor             | `engine/editor/`              | `lane/22-editor`              | B |
| 23-platform-win32     | `engine/platform/win32/`      | `lane/23-platform-win32`      | B |
| 24-platform-linux     | `engine/platform/linux/`      | `lane/24-platform-linux`      | B |
| 25-platform-macos     | `engine/platform/macos/`      | `lane/25-platform-macos`      | B |

The shared `engine/platform/CMakeLists.txt` (top-level platform dispatch) is **orchestrator-owned**; each platform lane owns only its OS-specific subdir.

Similarly, `engine/render/CMakeLists.txt` and `engine/physics/CMakeLists.txt` (the lane-aggregator CMakeLists for sublanes) are orchestrator-owned; lane agents own only their leaf subdir.

## Hard rules for every lane agent

These come on top of the design doc's coding standards (see DESIGN §14):

- **No `Co-Authored-By: Claude …` trailer** on any commit. The contributor list stays clean.
- **No exceptions / no RTTI in hot subsystems** (renderer, scene, physics, audio mixer, netcode hot path). DOTS contract per DESIGN §3.
- **No `vkAllocateMemory` / `[MTLDevice newBuffer*]` in the frame loop.** Allocator lint catches violations outside `engine/gpu/heap/`. See DESIGN §4.4 + §14.
- **No `std::shared_ptr` outside `engine/gpu/`** resource handles. The intrusive `psy::gpu::Handle<T>` is the only smart pointer in the engine runtime.
- **`-fno-fast-math`** on physics + netcode TUs. Determinism is mandatory for lockstep replay.
- **Real metric units.** 1 world unit = 1 metre. Real masses, real torques, real material strengths. No demo-scaled shortcuts.
- **Public headers are frozen** — `engine/<lane>/Public*.h` is the contract other lanes code against. Don't change without filing an Issue against the orchestrator; a public-header change cascades to every dependent lane and must be coordinated.
- You may freely edit internal headers (anything `_internal.h`, `Impl/*.h`, or `.cpp` files in your lane).
- **Catch2 `TEST_CASE` names must be ASCII-only.** ctest records each discovered test name and passes it back as a command-line filter; on Windows a non-ASCII name (a degree sign, an em dash, an arrow, …) is mangled by the CRT's legacy code-page argv decoding, so the filter matches nothing and the test is reported as *failed* even though the product is fine. Spell them out in ASCII instead (` deg`, `-`, `->`). This keeps `catch_discover_tests` robust on every toolchain — see the note in `tests/unit/CMakeLists.txt`.

## Mac smoke-test serialization

Multiple agents running the same Mac binary in parallel will saturate the platform layer (window, audio device, Vulkan surface / Metal layer present). Acquire an atomic lock via `mkdir` before any sample-binary or smoke invocation:

```bash
LOCK=/tmp/psynder_gx_smoke.lockdir
while ! mkdir "$LOCK" 2>/dev/null; do sleep 0.5; done
trap 'rmdir "$LOCK"' EXIT

# Run sample / smoke
build/mac-release/bin/sample_00_clear --smoke-frames=10 || true
```

Concurrent BUILDS (`cmake --build`) are fine — only serialize the runtime invocation.

## Commit-message rules

- Subject line: imperative, < 70 chars. Reference the lane Issue (e.g. `gpu: bring up Vulkan swapchain on Win/Linux (#7)`).
- Body: explain the *why*, not the *what*. Mention any cross-lane API changes (which require pre-approval).
- **No `Co-Authored-By: Claude Opus 4.7 …` trailer**. Strip it.

## CMake ownership

The top-level `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `cmake/*.cmake`, `.github/workflows/`, and the **aggregator** `engine/render/CMakeLists.txt`, `engine/physics/CMakeLists.txt`, `engine/platform/CMakeLists.txt`, `engine/net/CMakeLists.txt`, `engine/ui/CMakeLists.txt`, `engine/world/CMakeLists.txt`, `engine/editor/CMakeLists.txt` are owned by the orchestrator. Lane agents own *only their leaf subdirectory's* `CMakeLists.txt`. If your lane needs the top-level build wiring changed (new option, new dep), file an Issue against the orchestrator rather than editing the root files yourself.

## Public-header contracts (load-bearing)

Wave 0 froze the public API of every subsystem. Each `engine/<lane>/Public*.h` file is the **contract** other lanes code against. **Do not change public headers without filing an Issue against the orchestrator** — a public-header change cascades to every dependent lane and must be coordinated.

You may freely edit internal headers (anything `_internal.h`, `Impl/*.h`, or `.cpp` files in your lane).

## Integration branch

The orchestrator maintains `integration/wave-N` as a periodically-rebased branch carrying every in-flight lane PR, for live user testing. Lane PRs target `main`; the orchestrator merges your branch into the integration branch as you push. You don't need to interact with it.

## Mac vs Win/Linux validation

The orchestrator runs on macOS. PRs that touch Win32 or Linux paths land with a clear "needs PC validation" note rather than blocking on testing the agent can't perform. The user verifies their Windows / Linux boxes separately.

## Vulkan vs Metal symmetry

Lane 07 (gpu) is the unified Vulkan + Metal abstraction. Lanes that build on top (09 render-pipeline, 10 render-rt, 11 render-post, 21 ui) write **API-neutral code** that calls into `psy::gpu::*` — never `vk*` / `MTL*` directly. The platform lanes (23/24/25) own the surface-creation glue between OS windowing and `psy::gpu::Device`.

If your lane needs a feature missing from `psy::gpu::*` (e.g., a Vulkan extension or Metal-specific resource), file an Issue against lane 07. Do not bypass.
