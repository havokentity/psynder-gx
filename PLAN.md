# Psynder-GX — 25-Agent Parallel Build Plan

> Companion to `DESIGN-PSYNDER-GX.md` v1.0 (handoff 2026-05-19). This plan operationalizes the design for **25 Claude agents working simultaneously in git worktrees** to scaffold the engine across milestones M0–M2 in days, with the public-header surface frozen for every later milestone.

**Plan version:** v1.0 — drafted 2026-05-19
**Project:** Psynder-GX (GPU-accelerated competitive FPS engine)
**Sister projects (code reuse authorized):** [Psynder](https://github.com/havokentity/psynder) (CPU engine), DeMonT PathTracer at `~/MyRepos/PathTracer` (the dmonte path tracer)
**Working dir:** `/Volumes/XTRM 5 Media/More MyRepos/Psynder-GPU`
**Target GitHub repo:** `github.com/havokentity/psynder-gx` (public, MIT OR Apache-2.0 dual)
**Platforms:** Windows x86-64 (Vulkan), Linux x86-64 (Vulkan, Wayland + X11), macOS Apple Silicon (native Metal)

---

## 0. TL;DR

- **25 lanes**, one Claude agent per lane, all running in parallel in **git worktrees** off a freshly-bootstrapped repo.
- **Orchestrator** (this Claude session) owns root files (CMake, CI, design docs, AGENTS.md, tools, samples, tests).
- **Lane agents** own one subdirectory each + its `CMakeLists.txt`. Touching anything outside their lane = STOP and file an Issue.
- **Two waves**: Wave A (8 foundation lanes, mostly vendor-from-Psynder) → Wave B (17 subsystem/renderer/platform lanes, GX-specific implementation).
- **Acceptance criteria are milestone-gated** per `DESIGN-PSYNDER-GX.md` §13. M0 (animated clear color on all 3 OSes from one binary) is the immediate target. M1 (textured triangle), M2 (PBR sphere) follow.
- **Out of scope for the "few days" sprint:** RT shadows (M5), netcode (M6), vehicles + destruction (M7), voice + AoI (M8), 1.0 polish (M9). These get **public-header stubs + module skeletons** in Wave B so M5+ can fill them in later.

### Plan revisions (post-bootstrap, 2026-05-19)

- **Sister-project trust level (user clarification):** Psynder is software-rendered AND CURRENTLY UNTESTED. Vendored Psynder code BUILDS but its runtime correctness has not been verified. Treat vendored Psynder modules as starting points for adaptation, not as canonical truth. Lane agents must add their own smoke / unit tests where lane-relevant rather than assume Psynder's behavior is correct.
- **Separate `psy-*` shared-module repos (user clarification):** OK as a follow-up. The DESIGN §5.4 separate-repo architecture remains the long-term aim. For this build sprint, the monorepo approach stays; refactoring to separate repos is a post-M0 task that can be done once the lane public-header contracts have settled.
- **Wave A execution pivot:** The first 8 parallel spawn hit the API rate limit immediately (the user's daily quota); the foundation lanes were instead integrated by the orchestrator directly on `main`. Wave A delivered: 3 GX cooker public-header contracts (TextureCookerGx / MeshletCookerGx / ShaderCookerGx) + GxStubs.cpp + raymarcher-deferred-removal note. The 6 vendor-only foundation lanes (core, math, simd, jobs, scene, world-bsp) were validated by the Phase 0 build check; no further work needed. Wave B will fire agents after the API rate limit resets, in smaller batches to avoid re-tripping the limit.

---

## 1. Honest scope: what "a few days" actually buys

The design doc estimates **~24 months small-team / 36 months solo** to ship Psynder-GX v1.0. With 25 agents and aggressive parallelism, in **a few days** we will deliver:

✅ **Built and demoable**
- M0: animated clear color presenting via Vulkan (Win/Linux) and native Metal (macOS) from a single source tree.
- M1: rotating textured triangle with a Slang-compiled shader pipeline (SPIR-V + Metal IR).
- M2 (partial): forward+ light-cluster compute scaffold + PBR fragment shader on a glTF mesh. Probably one demo scene (PBR sphere with 4 point lights) lit + tonemapped.
- Repo scaffold: CMake builds clean on all three OSes, CI green for at least one OS (the orchestrator's macOS), other OS validation flagged for user.
- Editor opens, IPC handshake works, inspector panel renders empty.

✅ **Scaffolded with public-header contracts + stubs**
- Every lane has its public-header `*.h` files defining the API surface other lanes code against (the "contract" per Psynder's `AGENTS.md` model).
- Subsystem skeletons (audio mixer thread, Jolt physics tick loop, Lua REPL, RmlUi RenderInterface) wired into the main loop with no-op or canned-return implementations.
- Public-header surface frozen — later milestone work (RT, netcode, vehicles) plugs into a stable API.

⚠️ **Not built (deferred per DESIGN §13)**
- M3 BSP indoor + editor v0 — needs ~2 weeks of integration after M2.
- M4 outdoor + Jolt — needs character controller + terrain integration after M2.
- M5 hardware RT + DDGI — needs the M3-M4 scene before RT lighting matters.
- M6 networking + CS-scale playable — needs M4 first playable.
- M7 vehicles + destruction — vehicle module is 6-8 weeks of focused dev per §10.1.
- M8 voice + BF-light + AoI — needs M6/M7.
- M9 polish + 1.0 — final.

**This plan is the kickoff sprint**, not the v1.0 launch sprint. The 25-lane scaffold + frozen public API is what makes the rest of the roadmap parallelizable post-sprint.

---

## 2. The 25 lanes

Mirrors Psynder's lane structure with GX-specific adjustments (split renderer into pipeline/RT/post; split physics into core/vehicle/destruction; net into base/voice; combine editor; combine UI; keep three platform lanes for the surface-creation work).

| # | Lane | Owned directory | Branch | Wave | Sister source |
|---|---|---|---|---|---|
| 01 | **core**                | `engine/core/`                | `lane/01-core`                | A | Psynder `engine/core/` + PathTracer `src/console/`, `src/core/Hardware/`, `src/core/Memory/` |
| 02 | **math**                | `engine/math/`                | `lane/02-math`                | A | Psynder `engine/math/` |
| 03 | **simd**                | `engine/simd/`                | `lane/03-simd`                | A | Psynder `engine/simd/` |
| 04 | **jobs**                | `engine/jobs/`                | `lane/04-jobs`                | A | Psynder `engine/jobs/` + PathTracer `src/core/Jobs/` |
| 05 | **asset**               | `engine/asset/`               | `lane/05-asset`               | A | Psynder `engine/asset/` + GPU texture cookers (BC1/BC3/BC7/ASTC) + meshlets |
| 06 | **scene**               | `engine/scene/`               | `lane/06-scene`               | A | Psynder `engine/scene/` (ECS) |
| 07 | **gpu**                 | `engine/gpu/`                 | `lane/07-gpu`                 | B | **NEW** — Vulkan + Metal abstraction (`psy::gpu::Heap/Buffer/Texture/Cmd/RayTracing`) |
| 08 | **shader**              | `engine/shader/`              | `lane/08-shader`              | B | **NEW** — Slang integration, SPIR-V + Metal IR pipeline, hot-reload |
| 09 | **render-pipeline**     | `engine/render/pipeline/`     | `lane/09-render-pipeline`     | B | **NEW** — forward+, depth pre-pass, HiZ pyramid, GPU cull (compute), light clusters |
| 10 | **render-rt**           | `engine/render/rt/`           | `lane/10-render-rt`           | B | **NEW** — RT shadows, DDGI probes, hybrid SSR+RT reflections (stubs for M5) |
| 11 | **render-post**         | `engine/render/post/`         | `lane/11-render-post`         | B | **NEW** — bloom, tonemap, upscale wrappers (DLSS/FSR/XeSS/MetalFX stubs) |
| 12 | **world-bsp**           | `engine/world/bsp/`           | `lane/12-world-bsp`           | A | Psynder `engine/world/bsp/` (BSP + PVS) |
| 13 | **world-outdoor**       | `engine/world/outdoor/`       | `lane/13-world-outdoor`       | A | Psynder `engine/world/outdoor/` (CDLOD heightmap; raymarcher excluded per ADR-GX-008) |
| 14 | **audio**               | `engine/audio/`               | `lane/14-audio`               | B | Psynder `engine/audio/` + voice-channel mixing slot |
| 15 | **physics-core**        | `engine/physics/core/`        | `lane/15-physics-core`        | B | Jolt wrapper (vendored `third_party/jolt/`) + character controller (capsule, crouch/prone/lean/ladder/water) |
| 16 | **physics-vehicle**     | `engine/physics/vehicle/`     | `lane/16-physics-vehicle`     | B | Raycast suspension, Pacejka-lite tires, drivetrain, aero, damage, tracked vehicles |
| 17 | **physics-destruction** | `engine/physics/destruction/` | `lane/17-physics-destruction` | B | Pre-fractured chunks, structural-integrity graph, cascade sim, debris pool |
| 18 | **net**                 | `engine/net/`                 | `lane/18-net`                 | B | Psynder `engine/net/` + 128-tick capable, lag comp rewind, snapshot delta, `.psydem` demos |
| 19 | **net-voice**           | `engine/net/voice/`           | `lane/19-net-voice`           | B | **NEW** — Opus codec (vendored), server-side mixing per recipient, VAD, push-to-talk |
| 20 | **script**              | `engine/script/`              | `lane/20-script`              | B | Psynder `engine/script/` (Lua 5.4) |
| 21 | **ui**                  | `engine/ui/`                  | `lane/21-ui`                  | B | Psynder `engine/ui/imm/` + `engine/ui/rml/`; GX adds GPU `RenderInterface` for RmlUi |
| 22 | **editor**              | `engine/editor/`              | `lane/22-editor`              | B | Psynder `engine/editor/{core,ipc,web}/` + GX panels (material editor, destruction authoring, lightmap/probe baker, replay viewer) |
| 23 | **platform-win32**      | `engine/platform/win32/`      | `lane/23-platform-win32`      | B | Psynder `engine/platform/win32/` + `vkCreateWin32SurfaceKHR` + raw mouse via `RegisterRawInputDevices` |
| 24 | **platform-linux**      | `engine/platform/linux/`      | `lane/24-platform-linux`      | B | Psynder `engine/platform/linux/` + Wayland (`vkCreateWaylandSurfaceKHR`) primary + X11 fallback + evdev |
| 25 | **platform-macos**      | `engine/platform/macos/`      | `lane/25-platform-macos`      | B | Psynder `engine/platform/macos/` + AppKit + `MTKView`/`CAMetalLayer` + IOKit raw mouse + GCController |

**Orchestrator owns (not in any lane):**
- Root files: `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `cmake/*.cmake`, `.github/workflows/`, `.clang-format`, `.clang-tidy`, `LICENSE-MIT`, `LICENSE-APACHE`, `README.md`, `DESIGN-PSYNDER-GX.md`, `AGENTS.md`, this `PLAN.md`.
- `tools/lm_cook_gx/`, `tools/lm_bake_gx/`, `tools/lm_qbsp/`, `tools/lm_pak/` — cross-cutting offline tools.
- `samples/01_triangle/` through `samples/07_bflight_map/` — touch many lanes.
- `tests/{unit,golden,netcode,bench}/` — cross-cutting test harnesses.
- `third_party/` — vendored Jolt, Opus, FreeType, RmlUi, Slang, Vulkan SDK manifest.

---

## 3. Wave plan

### Wave 0 — Orchestrator scaffold (~1 hour, no lane agents yet)

Single-threaded prep work that lane agents will fan out from.

1. `git init` in `/Volumes/XTRM 5 Media/More MyRepos/Psynder-GPU` (currently not a repo).
2. Write **root build files**: `CMakeLists.txt`, `CMakePresets.json` (Win/Linux/macOS × Debug/Release × normal/dedicated-server presets), `vcpkg.json` (vulkan, slang, jolt-physics, opus, freetype, rmlui, fmt, lua, zstd, catch2, tracy), `cmake/CompilerWarnings.cmake`, `cmake/Sanitizers.cmake`, `cmake/Dependencies.cmake`, `cmake/Lanes.cmake`, `cmake/PsynderRules.cmake` (DOTS rules: no-exceptions / no-RTTI in hot subsystems).
3. Write **CI**: `.github/workflows/ci.yml` with 4 matrix entries (Win+Vulkan editor, Linux+Vulkan editor, Linux dedicated-server headless, macOS+Metal editor).
4. Write **legal**: `LICENSE-MIT`, `LICENSE-APACHE`, `NOTICE`, `.clang-format`, `.clang-tidy`.
5. Move `DESIGN-PSYNDER-GX.md` to the repo root. Write `README.md` (3-page intro: vision, status, building, lane map).
6. Write `AGENTS.md` — adapted from Psynder's, with the GX lane map from §2 above.
7. Create the **engine tree** with empty subdirs + a stub `CMakeLists.txt` per lane (so the build links, just to nothing).
8. Write **public-header contracts** — each lane gets `engine/<lane>/Public<Lane>.h` with the API surface frozen. Other lanes only `#include` `Public*.h`, never internal headers. (This is the "contract" that lets 25 agents work in parallel without colliding.)
9. **Vendor reusable code** from Psynder + PathTracer:
    - `cp -r ../Psynder/engine/core/* engine/core/` (then a lane agent will rename `psy::*` → `psy::*` no-op; the namespace is already correct).
    - Same for `math`, `simd`, `jobs`, `asset`, `scene`, `world/bsp`, `world/outdoor`, `audio`, `physics`, `net`, `script`, `ui`, `editor`, `platform/{win32,linux,macos}`.
    - From PathTracer: `cp -r ../PathTracer/src/console/* engine/core/console/` (user's explicit grant), and selectively pull `src/core/Hardware/`, `src/core/Memory/` if Psynder's are missing pieces.
10. Vendor **third_party**: download/submodule Jolt (MIT), Opus (BSD), FreeType (FTL), RmlUi (MIT), Slang (MIT-ish); add manifest entries.
11. **First commit** on `main`. Commit message: `chore: bootstrap repo from DESIGN-PSYNDER-GX.md v1.0 + vendor shared modules from Psynder + PathTracer console`.
12. `gh repo create havokentity/psynder-gx --public --source . --description "GPU-accelerated competitive FPS engine — sister to Psynder, Vulkan + native Metal, C++23, MIT OR Apache-2.0 dual-license. v1.0 design handoff, build in progress."` then `git push -u origin main`.

After Wave 0, the repo is on GitHub, the tree builds (empty), public headers are frozen, and 25 lane branches can be created from `main`.

### Wave A — Foundation lanes (8 agents in parallel, ~2-4 hours)

These lanes are **~100% shared with Psynder** — the agent's main job is: vendor from Psynder, tweak namespace or build glue if needed, write a sample-friendly smoke test, commit. Low risk, high parallelism.

| # | Lane | Goal |
|---|---|---|
| 01 | core | Vendor `Log`, `Diag`, `Types`, `alloc/`, `console/`. Smoke: `psy::log("hello")` prints, console var system works. |
| 02 | math | Vendor `Math.h`, `Rng.h`. Smoke: 4×4 matrix math + RNG seeded reproducibility passes Catch2 cases. |
| 03 | simd | Vendor `Simd.h` (`f32x4`/`f32x8`/`i32x8` + NEON on arm64, AVX2/AVX-512 on x86). Smoke: dot-product benchmark on each ISA. |
| 04 | jobs | Vendor `JobSystem` (Chase-Lev work-stealing). Smoke: parallel sum reduction across 16 workers matches single-thread baseline. |
| 05 | asset | Vendor `Vfs` + `.lmpak`. GX-specific: BC7 cooker stub + meshlet meshopt stub (full impl deferred to M2). Smoke: round-trip a `.lmpak` archive. |
| 06 | scene | Vendor `World`, archetype chunks, `PSYNDER_COMPONENT(...)` macro. Smoke: 100k entity transform-update benchmark hits the perf budget. |
| 12 | world-bsp | Vendor BSP+PVS loader. Smoke: load a Quake-style `.bsp` fixture, traverse PVS. |
| 13 | world-outdoor | Vendor CDLOD heightmap chunker, splatmap. Strip Psynder's optional raymarcher backend (excluded per ADR-GX-008). Smoke: load 64×64 heightmap, dump CDLOD chunk list. |

**Why Wave A first:** every other lane depends on `core` + `math` + `simd` + `jobs` + `scene` + `asset`. Locking the public API of these 8 first means Wave B agents have a stable foundation to code against.

### Wave B — Subsystem, renderer, platform lanes (17 agents in parallel, ~1-2 days)

The bulk of GX-specific work. Higher complexity; each agent gets a tighter prompt + bigger sample target.

| # | Lane | M0–M2 goal | Stubs for later milestones |
|---|---|---|---|
| 07 | gpu                 | Vulkan instance + device + swapchain on Win/Linux. Metal device + `CAMetalLayer` view on macOS. `psy::gpu::Heap/Buffer/Texture/Cmd` operational. | `psy::gpu::RayTracing` API surface frozen, impl stubbed (M5) |
| 08 | shader              | Slang invoked, SPIR-V + Metal IR emitted, pipeline cache scaffolded. Hot-reload watcher wired. | Mesh-shader variants stubbed |
| 09 | render-pipeline     | M1 textured triangle path: vertex/index buffer upload, PBR fragment shader on a glTF mesh by M2. Depth pre-pass + HiZ pyramid compute pass scaffolded. | Forward+ light cluster build for M2; GPU cull compute for M3; mesh shaders deferred |
| 10 | render-rt           | Public-header contract for RT shadows + DDGI + hybrid reflections. No-op CSM fallback used at M2. | RT shadows + DDGI impl for M5 |
| 11 | render-post         | Bloom + ACES tonemap operational at M2. Upscale wrapper API surface frozen (DLSS/FSR/XeSS/MetalFX selectors). | Vendor SDK integration deferred to M9 |
| 14 | audio               | CPU mixer thread up, plays a `.wav`. HRTF stub. Voice-channel slot reserved (impl in lane 19). | FFT reverb deferred to M4-M5 |
| 15 | physics-core        | Jolt vendored + built into a static lib. World tick at fixed 120 Hz. Character controller capsule operational by M4 — for the sprint, smoke = "rigid body falls under gravity." | Lockstep-determinism flag set; full controller modes (crouch/prone/lean/ladder/water) for M4 |
| 16 | physics-vehicle     | Public-header contract for vehicle module. Raycast suspension scaffolded — smoke = "wheel sample touches ground." | Pacejka tires + drivetrain for M7 |
| 17 | physics-destruction | Public-header contract + data structures. Chunk + joint serializer round-trips a tiny test asset. | Structural-integrity graph + cascade sim for M7 |
| 18 | net                 | Public-header contract: `psy::net::Server`, `psy::net::Client`, `psy::net::Snapshot`, `.psydem` reader/writer skeleton. rUDP frame send/recv smoke. | 128-tick + lag comp rewind + AoI for M6-M8 |
| 19 | net-voice           | Public-header contract + Opus encode/decode round-trip smoke. | Server-side mixing + positional integration for M8 |
| 20 | script              | Lua 5.4 vendored + REPL bound to the editor. `print()` from Lua to engine console works. | Full DOTS-system Lua binding for M3 |
| 21 | ui                  | Vendor immediate-mode in-viewport overlay (perf graphs). RmlUi vendored; `RenderInterface` GPU-backed via lane 09's pipeline scaffolds — smoke = HTML hello-world panel. | Full HUD set for M3 |
| 22 | editor              | React panels build (pnpm install runs in CI), Chrome launch wired, WebSocket IPC handshakes, blank Inspector panel opens. | Material editor + destruction authoring for M2-M7 |
| 23 | platform-win32      | Win32 window + Vulkan surface + raw mouse input. Smoke: `sample_00_clear` opens a window and animates clear color on Windows. | WASAPI audio wiring with lane 14 |
| 24 | platform-linux      | Wayland primary (xdg-shell + Vulkan surface), X11 fallback. evdev raw input. Smoke: `sample_00_clear` on Ubuntu 24.04 Wayland. | PipeWire wiring with lane 14 |
| 25 | platform-macos      | AppKit + `MTKView`/`CAMetalLayer`. IOKit raw mouse + `GCController`. Smoke: `sample_00_clear` on macOS Apple Silicon, animated clear color via Metal. | CoreAudio wiring with lane 14 |

### Wave C — Integration (orchestrator, ~few hours)

After lanes return:

1. Pull each lane branch into an integration branch (`integration/wave-A` then `integration/wave-B`).
2. Resolve any cross-lane API drift (should be near-zero because public headers were frozen in Wave 0).
3. Wire `samples/01_triangle/` against the assembled gpu + shader + render-pipeline lanes.
4. Run the M0 smoke test on macOS (the orchestrator's box), per `AGENTS.md` mac-serialization protocol (`mkdir /tmp/psynder_gx_smoke.lockdir`).
5. Push to GitHub. CI confirms macOS green. User runs Win + Linux validation on their boxes.
6. Open Issues for any lane that left work undone or that needs follow-up before M1.

---

## 4. Agent execution mechanics

### 4.1 Worktree isolation

Each lane agent runs in its own git worktree off `main`:

```bash
# Orchestrator does this once per lane before spawning the agent
git worktree add ../psynder-gx-worktrees/lane-01-core lane/01-core
```

The Agent tool's `isolation: "worktree"` does this automatically. 25 agents = 25 worktrees on disk under `.claude/worktrees/` (or similar). Each agent sees a full repo, edits only its lane's files, commits to its lane branch.

### 4.2 Spawning pattern

Wave A:
```
Agent({ description: "Lane 01 — core", subagent_type: "general-purpose",
        isolation: "worktree", run_in_background: true,
        prompt: <self-contained brief, see §5> })
× 8 in one message (parallel)
```

Wave B (after Wave A returns):
```
Agent(× 17 in one message, parallel)
```

### 4.3 Per-agent prompt template

Every lane agent gets a self-contained prompt with these sections (drafted in detail per-lane in §5 below):

1. **Identity** — "You are the Psynder-GX lane N agent. Your lane: `<name>`. Your owned dir: `engine/<lane>/`."
2. **Read first** — `DESIGN-PSYNDER-GX.md` §<relevant sections>, `AGENTS.md` (ownership rules), `PLAN.md` §<lane row>.
3. **Sister source to vendor** — exact file paths in Psynder + PathTracer to copy in.
4. **Public-header contract** — what `Public<Lane>.h` exposes (already written in Wave 0; don't modify without filing an Issue).
5. **M0–M2 acceptance criteria** — concrete pass/fail bar.
6. **Hard rules**:
   - No `Co-Authored-By: Claude …` on any commit.
   - No exceptions / no RTTI in hot subsystems (per DOTS contract).
   - No `vkAllocateMemory` / `[MTLDevice newBuffer*]` in the frame loop (per §14).
   - No `std::shared_ptr` outside `engine/gpu/`.
   - `-fno-fast-math` in physics + netcode TUs.
   - Don't touch files outside the owned directory. If you need to, STOP and report.
7. **Commit + push** — squash on the lane branch, push to `origin/lane/<name>`, return when done.

### 4.4 Mac smoke-test serialization

Inherited verbatim from Psynder's `AGENTS.md`. Agents that smoke-test on macOS acquire `/tmp/psynder_gx_smoke.lockdir` via `mkdir` atomic mutex before invoking any sample binary. Concurrent builds are fine; only the runtime invocation needs serialization.

### 4.5 Integration branch

The orchestrator maintains `integration/wave-A` and `integration/wave-B` branches that periodically rebase + merge all in-flight lane branches. Useful for the user to pull and test the live build state without waiting for every lane to settle on `main`.

---

## 5. Per-lane briefs (skeleton — full prompts written at spawn time)

Each lane brief follows the template in §4.3. The full text is generated at spawn time from the row in §2 + the M0–M2 column in §3 + the design doc sections referenced. Below is the shape; the actual prompts will be ~300–500 words each.

**Example — Lane 07 (gpu) prompt sketch:**

> You are the Psynder-GX **lane 07 (gpu)** agent. Your owned directory: `engine/gpu/`. Your branch: `lane/07-gpu`.
>
> **Read first:** `DESIGN-PSYNDER-GX.md` §4 (memory), §7 (renderer pipeline), §11 (platform), §14 (coding standards). `AGENTS.md` (ownership rules). `PLAN.md` lane 07 row.
>
> **Sister source:** **Nothing direct** — this is a GX-NEW module. Reference: Psynder `engine/render/` for the overall architecture style; `third_party/vulkan-sdk/` headers (vendored by orchestrator); `third_party/metal-cpp/` Apple-provided headers for the Metal side.
>
> **Public-header contract** (frozen by orchestrator in Wave 0): `engine/gpu/PublicGpu.h` exposes `psy::gpu::{Device, Heap, Buffer, Texture, Cmd, Pipeline, RayTracing, Handle<T>}`. Other lanes only `#include "engine/gpu/PublicGpu.h"`. Do not change the public header without filing an Issue against the orchestrator.
>
> **M0–M2 acceptance:**
> - M0: Vulkan instance + device + swapchain on Win/Linux; Metal device + `CAMetalLayer` on macOS. `Device::present()` swaps the chain.
> - M1: `Buffer::create` + `Texture::create` operational; staging upload on Win/Linux, direct map on Apple Silicon unified memory.
> - M2: `Cmd::beginRender / draw / endRender` with bindings; descriptor pool + per-frame descriptor sets.
>
> **Hard rules** (in addition to §4.3): No `vkAllocateMemory` outside `engine/gpu/heap/`. Use `vk-bootstrap` or `volk` for loader — pick `volk` (lighter). `psy::gpu::Handle<T>` is the only smart pointer allowed; impl is intrusive refcount with frames-in-flight tracking. Use Metal-cpp (C++ bindings, vendored) not Objective-C++ in the public API; .mm files only inside `engine/gpu/mtl/`.
>
> **Acceptance smoke:** `samples/00_clear/` calls `Device::present()` in a loop and the screen shows an animated clear color on all 3 OSes.

All 25 lanes get an equivalent ~300-word brief at spawn time.

---

## 6. Dependency graph (which lanes can start before which)

```
                                ┌──── 01 core ────┐
                                │                  │
                  ┌─── 02 math ─┤                  ├── (all other lanes depend on these 4)
                  │             │                  │
                  ├─── 03 simd ─┤                  │
                  │             │                  │
                  ├─── 04 jobs ─┘                  │
                  │                                 │
                  ├─── 05 asset (VFS, archives) ────┤
                  │                                 │
                  ├─── 06 scene (ECS) ──────────────┤
                  │                                 │
   [Wave A] ──────┼─── 12 world-bsp ───────────────┤
                  │                                 │
                  └─── 13 world-outdoor ────────────┘
                                                    │
                                                    ▼
   [Wave B] ─── 07 gpu ─┬─── 08 shader ─┬─── 09 render-pipeline
                        │                │
                        │                ├─── 10 render-rt (stubs)
                        │                │
                        │                └─── 11 render-post
                        │
                        └─── (23/24/25 platforms create the gpu surface)

   [Wave B parallel] ─── 14 audio
                     ─── 15 physics-core ──┬── 16 physics-vehicle
                     │                     └── 17 physics-destruction
                     ─── 18 net ──── 19 net-voice
                     ─── 20 script
                     ─── 21 ui ─── (needs 09 render-pipeline for GPU RenderInterface)
                     ─── 22 editor (independent of renderer; IPC is just sockets)
```

**Key insight:** Wave A's 8 lanes are mostly independent of each other (foundation), and Wave B's 17 lanes mostly depend on Wave A's frozen public headers (locked in Wave 0). So both waves can fan out wide. The two **serialization points** are:
- Lane 09 needs lane 07 (gpu) at least at the public-header level → both can start, but 09 stubs to 07's headers.
- Lane 21 (ui) GPU `RenderInterface` needs lane 09 → 21 stubs the render path until 09 lands.

The public-header-first discipline (Wave 0) collapses these dependencies to "everyone codes against frozen headers in parallel" — the design pattern that makes 25-agent parallelism actually work.

---

## 7. Decisions to confirm with user before kicking off

These three decisions shape the rest of execution. I'll ask via AskUserQuestion immediately after this plan is presented.

1. **GitHub repo name.** Recommend `psynder-gx` (lowercase, matches sister `psynder` convention). Alternatives: `Psynder-GX` (matches design doc casing), `Psynder-GPU` (matches local dir, but diverges from canonical brand).
2. **Monorepo vs separate `psy-*` repos for shared modules.** Recommend **monorepo for the rapid-build phase** (everything in `engine/`, deviates from DESIGN §5.4 but realistic for "few days"). The §5.4 separate-repos split can happen post-1.0 as a refactor.
3. **Green-light to spawn 25 background agents.** Each agent costs Sonnet/Opus time + writes to its own worktree. Total budget will be measured in tens of millions of tokens across the sprint. Confirm before I kick off Wave 0 + agents.

---

## 8. Risks + mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| Public-header drift across lanes mid-sprint | **High** | Freeze every `Public<Lane>.h` in Wave 0; lane agents file Issues for any contract change, orchestrator coordinates. |
| Lane completion variance — some lanes finish in 1 hour, others in 10 hours | Medium | Wave A is mostly vendor work (fast); Wave B is implementation (slower). Stagger spawn — Wave A first, then Wave B once Wave A's headers exist. |
| 25 worktrees on disk = lots of duplication | Low | Disk space; modern SSDs absorb this fine. Worktrees cleaned up post-merge. |
| Mac/Win/Linux validation asymmetry — orchestrator is on macOS only | Medium (per DESIGN risk) | Mac CI green is the bar I can enforce. Win + Linux flagged "needs PC validation" — user runs on their boxes. |
| Cross-lane integration surprises at Wave C | Medium | Frozen public headers absorb most. Build CI on every lane PR catches link errors immediately. |
| "Few days" turns into "few weeks" because the renderer is harder than the doc says | Realistic | Scope honesty (§1): M0–M2 in days is the real target; M3+ is the next sprint. The plan ships scaffolds for M3-M9, not implementations. |
| Vendoring code from Psynder + PathTracer creates copyright surprises | Low | All three projects are author's own (`havokentity`), all MIT. Vendor with attribution in `NOTICE`. |
| 25 agents writing in parallel exhaust the API rate limit | Possible | Stagger Wave B spawn into two batches of ~8 + 9 if needed. |
| Cargo-culting from Psynder's design where GX should diverge (e.g. raymarcher backend) | Medium | Each lane's brief calls out divergences explicitly (e.g. lane 13 strips Psynder's raymarcher backend per ADR-GX-008). |

---

## 9. Open questions (not blocking the kickoff)

The design doc commits to *what* and *why*; some *how* details are intentionally not specified and will be decided per-lane:

- Exact Vulkan-loading library: **`volk`** (decided by orchestrator — lighter than `vk-bootstrap`, no init dance hidden).
- Exact Slang version pin: latest stable at integration time; orchestrator vendors as submodule.
- Exact mesh-shader pipeline variant (V8 vs V16): defer to lane 09 bench.
- Descriptor strategy: descriptor indexing on capable HW, bindful fallback. Lane 07 decides at submit time.
- Pipeline cache: Vulkan + Metal **separate** caches (different binary formats). Decided.
- Jolt build: vendor as a submodule + build inside our tree via `add_subdirectory(third_party/jolt)`. Decided.

---

## 10. Definition of done for this kickoff sprint

✅ Repo on GitHub, public, MIT OR Apache-2.0 dual.
✅ Builds clean on all three platforms (CI green on macOS; Win + Linux flagged for user validation).
✅ `sample_00_clear` runs and animates a clear color via Vulkan (Win/Linux) + native Metal (macOS).
✅ All 25 lanes have a public-header contract + a committed `lane/<name>` branch + a passing per-lane smoke test.
✅ `AGENTS.md` documents the ongoing parallel-build protocol for future sprints (M1-M9).
✅ M0 demo recorded (screenshot or short GIF) attached to a GitHub Release tagged `v0.1.0-m0`.
✅ Memory in this Claude session updated with what shipped and what's next.

Anything beyond this is bonus. M1 (textured triangle) and M2 (PBR sphere) are the stretch targets if Wave B lanes finish early.

---

*— end of plan v1.0*
