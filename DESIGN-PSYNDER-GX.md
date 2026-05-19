# Psynder-GX — Lightweight GPU-Accelerated Competitive FPS Engine

> ## Mantra
> *Best GPU opts. Best CPU opts. FPS-centric and focused. Latest rendering quality. **HIGHLY OPTIMIZED for HIGH FPS.***

> *"The engine you'd actually pick for a serious indie competitive shooter."*

**Name:** **Psynder-GX** — *GX* for "Graphics eXtreme" / Game-eXtreme; the GPU-accelerated sister engine to **Psynder** (CPU software renderer). Same family, same DOTS contract, same React/Chrome editor, same Lua scripting, same target platforms — different renderer, different physics core, different netcode tuning.

**Targets:** CS:GO + Battlefield-latest gameplay; competitive FPS-centric with vehicles and destruction.

- **Map cap:** 2 km² (~1.4 km × 1.4 km)
- **Player cap:** 64
- **Tick rate:** 128 capable, 64 default for v1, 128 standard for competitive
- **First milestone:** CS-scale (10v10, small maps), scaling up to BF-light (32v32, 2 km² with vehicles + destruction) in later milestones

**Status:** Design doc, **v1.0 — handoff** (2026-05-19). All architectural decisions called; ready for implementation kickoff in parallel with Psynder. See §19 for kickoff notes.
**License (proposed):** MIT OR Apache-2.0 dual-license, with assets under CC-BY-4.0
**Language:** C++23
**Targets:** Windows (x86-64), Linux (x86-64), macOS (Apple Silicon, arm64)
**Sister project:** **Psynder** (CPU software renderer) — see `DESIGN.md`. Shared modules described in §5.

---

## 1. Vision

Psynder-GX is the **GPU-accelerated competitive FPS engine** in the Psynder family. The goal is a *lightweight, open-source engine you'd genuinely pick over Unity or Unreal* for a serious indie or pro-indie competitive shooter — not because it does more, but because it does less, faster, and with cleaner architecture.

We are not building Unreal. We are building the parts of Unreal that ship a competitive shooter, without the parts that ship Fortnite metaverse content.

### Design pillars

1. **Crazy-optimized.** GPU-driven rendering pipeline, forward+ shading, hardware RT where available, mesh shaders where supported. DOTS data layout. Job-graph parallelism on CPU side. Vulkan-as-thin-abstraction (no engine-side resource caching nonsense).
2. **Competitive FPS-first.** 128-tick netcode capability, server-authoritative architecture, lag compensation, client-side prediction, demo/replay recording. CS:GO-grade input handling and hitreg.
3. **DOTS for users, pragmatic-DOTS internals.** Same contract as Psynder (see `DESIGN.md` §3). The split makes contributions composable across both engines.
4. **Shared family architecture.** Math, ECS, physics (Jolt-based), audio, scripting, networking base, editor, asset pipeline, platform shim are **shared with Psynder** as versioned modules (§5). Only the renderer, GPU memory, GPU asset extensions, GPU platform surfaces, and netcode-at-scale tuning are GX-specific.
5. **Three platforms, first-class.** Windows / Linux / macOS Apple Silicon. **Vulkan** on Windows + Linux. **Native Metal** on macOS Apple Silicon in v1.0 (not MoltenVK).
6. **Open at every layer.** Engine, gameplay, destruction tech, networking, editor — all MIT/Apache OSS. Author can keep commercial art assets in a separate private repo.

### Non-goals

- **No D3D12 backend.** Vulkan is the Windows target. Skipping D3D12 simplifies the codebase and is fine on modern AMD/NVIDIA Windows drivers.
- **No MoltenVK on macOS for v1.0.** Native Metal pays off on Apple Silicon (one of our three first-class platforms).
- **No Nanite-style virtualized geometry.** R&D project on its own; out of scope for v1.
- **No Lumen-tier full GI.** DDGI probe-based GI is plenty.
- **No voxel destruction.** Chunk-based pre-fractured destruction handles our scale.
- **No cloth / fluids / soft bodies in v1.** Defer to v2.
- **No full flight simulation.** Helicopters and jets are kinematic / scripted movers, not aero-simmed.
- **No anti-cheat shipping in v1.** Architecture supports EAC / BattlEye / Vanguard SDK integration later; we don't build the anti-cheat ourselves.
- **No streaming open world.** Maps are level-scope, load as a unit. (At 2 km² this is trivial — full map fits in memory.)
- **No mobile.** Battery and thermal envelopes don't match the design.
- **No browser/WASM target.** Out of scope.

### Reference games

| Game | Year | What we steal |
|---|---|---|
| *Counter-Strike: Global Offensive* | 2012 | 64–128 tick netcode, hitreg precision, weapon feel, demo format, server-auth architecture |
| *Counter-Strike 2 / Source 2* | 2023 | GPU-driven rendering, hybrid sub-tick movement, animation refinements |
| *Battlefield 3 / 4* | 2011/13 | Vehicle gameplay, 64-player map design, destruction-as-design |
| *Battlefield 2042* | 2021 | Scale ceiling reference (we go *smaller* and tighter) |
| *Squad / Insurgency* | 2018+ | 2 km²-tier tactical FPS scale, vehicles, communication-driven gameplay |
| *Hell Let Loose* | 2019 | Large-map FPS with focused player count |

---

## 2. Technology choices

### 2.1 Language: C++23

Same standard as Psynder (see `DESIGN.md` §2.1) — Clang ≥ 17 primary on all three platforms, MSVC 19.40+ Windows secondary. Same constraint set (no exceptions in hot paths, no RTTI, no virtual in hot loops, etc.).

### 2.2 Build system

Same as Psynder (`DESIGN.md` §2.2): CMake ≥ 3.28 with presets per (OS × arch × config), Ninja generator, vcpkg manifest mode, clang-format + clang-tidy + iwyu in CI.

**Additional dev-environment requirements for GX:**
- Vulkan SDK (LunarG) on Windows and Linux.
- Xcode + Metal SDK on macOS.
- Slang compiler (or fallback to glslc + spirv-cross) for shader compilation.
- Node.js ≥ 20 + pnpm for the editor's React panels (shared with Psynder, see §10.8).

### 2.3 SIMD strategy

Same as Psynder (`DESIGN.md` §2.3). The CPU-side game loop, ECS iteration, physics, audio, and netcode all benefit from the same SIMD abstraction. GPU work doesn't use it — shaders have their own ISA.

### 2.4 Threading & parallelism

Same job-graph approach as Psynder (`DESIGN.md` §2.4) — Chase-Lev work-stealing per worker, one worker per physical core. **New for GX:** the rendering thread is *not* its own thread; rendering jobs go on the same worker pool as everything else. The Vulkan command-buffer-recording happens in parallel across workers (each tile's draw commands recorded by the worker that owns it).

---

## 3. Architecture mandate — DOTS

Same contract as Psynder (`DESIGN.md` §3) — strict DOTS for `render/`, `scene/ecs/`, `physics/`, `audio/mixer/`; pragmatic-DOTS for tools, editor, parsers, loaders, platform shim; strict DOTS for the public game/script API.

The DOTS rules transfer cleanly to GPU rendering. Components remain POD structs. Systems remain free functions over component arrays. The change is that the renderer's hot path now batches **GPU draw commands** instead of CPU rasterization triangles — but the ECS-to-render-list pipeline is identical.

`PSYNDER_COMPONENT(...)` macro and ECS chunk layout are shared with Psynder via the `psy-scene` module (§5).

---

## 4. Memory management

CPU-side memory follows Psynder's `dt::mem` / `psy::mem` hierarchy (see `DESIGN.md` §4) — `LinearArena`, `StackArena`, `TypedPool`, `BuddyAllocator`, `PageAllocator`, all with the same hugepage + NUMA + per-worker discipline. Shared module: `psy-core/mem`.

**GX-specific extensions:**

### 4.1 GPU resource heaps

The Vulkan / Metal layer manages a separate hierarchy of **device-memory allocators** parallel to the CPU side:

| Allocator | Memory type | Lifetime | Used by |
|---|---|---|---|
| `GpuDeviceLocal` | VRAM (or unified on Apple Silicon) | App / level | Vertex buffers, index buffers, textures, BVH (RT), framebuffers, render targets |
| `GpuHostVisible` | CPU-visible mapped device memory | App / per-frame staging | Staging buffers for uploads, per-frame uniform buffers |
| `GpuTransient` | Memoryless or aliased | Per-frame | G-buffer pass attachments (where applicable), transient render targets |
| `GpuDescriptorPool` | Descriptor sets | Per-frame or persistent | Descriptor sets per material / per frame |
| `GpuCommandPool` | Command-buffer storage | Per-frame, per-worker | Reset at frame end |

Both Vulkan and Metal expose this with different terminology (`VkDeviceMemory` + `VkBuffer` / `VkImage` vs Metal heaps and resources). Our abstraction (`psy::gpu::Heap`, `psy::gpu::Buffer`, `psy::gpu::Texture`) hides the API difference.

### 4.2 Apple Silicon unified memory

On macOS Apple Silicon, VRAM and CPU RAM are the same physical memory (unified architecture). The allocator detects this at startup and skips the staging-buffer-upload step — buffers can be mapped and written directly. This is a meaningful perf win on M-series and is handled transparently by the `GpuBuffer::map()` API.

### 4.3 Resource lifetime tracking

GPU resources are reference-counted via the `psy::gpu::Handle<T>` smart-handle pattern. **`std::shared_ptr` is still banned in the engine runtime** (per DOTS rule); the handle is a custom intrusive refcount that tracks frames-in-flight and only frees when the GPU is finished with the resource. This is the one place where retained-mode resource lifetimes leak into the otherwise DOTS-flat design — and it's a real necessity, not a convenience.

### 4.4 No mid-frame GPU allocations

Same principle as the CPU side: no `vkAllocateMemory` or `MTLDevice::newBufferWithLength` in the frame loop. All GPU resources are allocated at level load or startup, sized from a per-level budget. Render targets are aliased across passes via the transient allocator.

---

## 5. Relationship to Psynder — shared modules

Psynder-GX and Psynder are **sister engines** with a shared family of foundation modules. Each module is its own git repository, versioned independently, consumed by both engines as a pinned submodule (or vcpkg port).

### 5.1 Shared modules

| Module | Contents | Shared with Psynder |
|---|---|---|
| `psy-core` | math, simd, jobs, allocators (`psy::mem`), containers, logging, fixed-precision types | 100% |
| `psy-scene` | ECS (archetype-chunked), transforms, world partitions, `PSYNDER_COMPONENT(...)` macro | 100% |
| `psy-phys` | physics core (Jolt-based), rigid bodies, character controller, **chunk-destruction module**, **vehicle module** | 100% — both engines use the same physics |
| `psy-audio` | CPU mixer, HRTF, reverb, voice-chat mixing | 100% |
| `psy-net` | rUDP base, snapshot delta compression, demo recording, Lua bindings | ~90% — GX extends with 128-tick + AoI + lag comp tuning |
| `psy-script` | Lua 5.4 bindings, DOTS-compatible system registration | 100% |
| `psy-asset` | VFS, `.lmpak` archives, base file formats, cooker framework | ~85% — GX adds GPU-aware texture cook (BC1/BC3/BC7/ASTC) and shader compile step |
| `psy-editor` | React + TypeScript editor panels, WebSocket IPC server, msgpack protocol, IDL stub generator | 100% — the editor is renderer-agnostic |
| `psy-ui-rml` | RmlUi binding for player HUDs / menus | ~95% — GX uses GPU-accelerated RmlUi RenderInterface |
| `psy-platform` | window abstraction, input | ~70% — GX adds Vulkan / Metal surface creation; Psynder uses framebuffer present |

### 5.2 GX-specific modules

| Module | Contents |
|---|---|
| `psy-render-gx` | Vulkan + native Metal backend, forward+ pipeline, hardware RT, DDGI, mesh shaders, GPU-driven culling, temporal upscaling integration |
| `psy-gpu` | Vulkan/Metal abstraction (`psy::gpu`), device-memory allocators, command-buffer recording, descriptor management |
| `psy-shader` | Slang shader compiler integration, SPIR-V / Metal IR output, shader hot-reload |

### 5.3 Module versioning policy

- Each shared module follows semantic versioning.
- Engines pin to specific module versions in their `vcpkg.json` manifest.
- Breaking changes to shared modules require an ADR and a coordinated bump across both engines.
- The DOTS contract is *part of the shared module API*; breaking it is a major-version bump.

### 5.4 Why this isn't a monorepo

- Independent CI / release cadence per module.
- Smaller checkout for someone working on only one engine.
- Cleaner story for external users adopting just a shared module (e.g., someone who wants `psy-scene` for their own engine).
- Avoids the "Psynder vs Psynder-GX which is canonical" identity problem — neither is; the shared family is.

---

## 6. Architecture overview

```
+----------------------------------------------------+
|                    Game layer                      |
|   (CS-style 10v10 rules, BF-light 32v32 rules)     |
+----------------+----------------+------------------+
                 |                |
+----------------v---+  +---------v-------------------+
|  Scene / ECS       |  |   Asset / VFS / GPU cook    |
|  (psy-scene)       |  |   (.lmpak + BC textures)    |
+----------------+---+  +----+------------------------+
                 |           |
+----------------v-----------v--------------------+
|              Renderer (psy-render-gx)           |
|  +-----------+ +---------+ +---------+ +------+ |
|  | GPU cull  | | Forward+| | RT pass | | Post | |
|  | + dispatch| | shading | |  (opt)  | |      | |
|  +-----------+ +---------+ +---------+ +------+ |
|         ^             ^         ^         ^     |
|  +------+------+ +----+-----+ +-+--------+ |    |
|  | psy-gpu     | | DDGI     | | shader   | |    |
|  | (Vulkan /   | | probes   | | hot rld  | |    |
|  |  Metal)     | |          | |          | |    |
|  +-------------+ +----------+ +----------+ |    |
+--------------------------------------------+----+
                                              |
+---------------------------------------------v---+
|     Platform abstraction (psy-platform)         |
|     Vulkan surface / Metal layer, input, audio  |
+-------------------------------------------------+
```

Subsystems depend strictly upward, same as Psynder.

---

## 7. The renderer — `psy-render-gx`

GPU-driven, forward+-shaded, hardware-RT-accelerated, designed for competitive FPS perf budgets.

### 7.1 Pipeline at a glance

```
Per frame:
  1. CPU: ECS extract — collect visible draws via shared psy-scene queries
  2. CPU: per-frame uniform upload, instance buffer build
  3. GPU: depth pre-pass (early Z, fills HiZ pyramid)
  4. GPU: GPU-driven culling (compute) — frustum + occlusion (HiZ) per instance
  5. GPU: visible-instance buffer + indirect draw arguments produced on-GPU
  6. GPU: forward+ tile lighting setup (compute) — light cluster build
  7. GPU: opaque pass (forward+ shading, RT shadows where applicable, DDGI sample)
  8. GPU: hybrid SSR + RT reflections (compute)
  9. GPU: transparent pass (sorted, forward+)
 10. GPU: post — bloom, tonemap, temporal upscale (DLSS/FSR/XeSS/MetalFX)
 11. GPU: UI compositing (RmlUi + in-viewport ImGui-style overlays)
 12. GPU: present
```

The renderer is **GPU-driven** — culling, draw-call generation, and lighting cluster build happen on the GPU. The CPU side mostly extracts ECS state into per-frame buffers and submits indirect commands.

### 7.2 GPU-driven rendering

- **Instance descriptor buffer** holds every renderable instance in the scene (transform, material id, mesh id, mesh shader pipeline id). Built once at level load, deltas uploaded per frame for moving entities.
- **Compute cull pass** runs once per view (main camera, shadow cascades, RT acceleration structure update). Per-instance frustum + HiZ occlusion test. Output: indirect draw arguments + visible-instance index buffer.
- **Indirect draws** dispatched against the visible-instance buffer. The CPU never iterates renderables for culling.
- **Mesh shaders** on supported hardware (Turing+, RDNA2+, M3+) — emit primitives directly from compute-style shaders, skip vertex assembly. Legacy vertex/index pipeline as fallback on older HW.

### 7.3 Forward+ shading

Forward+ (clustered forward) instead of deferred. Reasons:

- Composes cleanly with hardware RT (no G-buffer to encode/decode).
- MSAA works trivially (deferred makes MSAA expensive).
- Lower bandwidth than deferred at our 1080p–1440p target resolutions.
- Transparency uses the same shader path; no separate forward pass for translucents.

Light clustering:
- View frustum subdivided into 3D clusters (typically 16 × 9 × 24 = 3,456 clusters at 1080p).
- Compute pass per frame: cluster-vs-light assignment.
- Each fragment looks up its cluster, iterates lights in that cluster.
- Supports up to ~256 dynamic lights per scene with negligible cost.

### 7.4 Hardware ray-traced shadows

- Hardware RT pipeline on supported HW: NVIDIA RTX 20+ (DXR / VK_KHR_ray_tracing), AMD RDNA2+, Apple M3+ (Metal 4 RT). All exposed through a unified `psy::gpu::RayTracing` abstraction.
- Sun shadow: 1 shadow ray per fragment, denoised (à-trous or temporal accumulation).
- Dynamic-light shadows: shadow rays per active light, packet-traced.
- **Fallback for non-RT HW:** cascaded shadow maps (4 cascades, PCF filtering). Auto-detected at startup; same shader path uses either RT or CSM via specialization constants.

### 7.5 Real-time global illumination — DDGI

Dynamic Diffuse Global Illumination (probe-based):

- 3D grid of irradiance probes (typically 16 × 8 × 16 = 2,048 probes per level).
- Each probe stores irradiance + visibility from all directions (octahedral mapping).
- Per-frame: a subset of probes is updated via RT trace from probe → world (using the same RT acceleration structure as shadows).
- Per-fragment: sample 8 nearest probes, weight by visibility.
- Cost: ~0.5 ms on RT-capable HW for the probe update pass; negligible at sample time.
- Fallback (non-RT HW): bake probes offline via `lm_bake`-equivalent, static irradiance only.

### 7.6 Hybrid reflections — SSR + RT

- Screen-space reflections (SSR) as primary: cheap, captures most cases.
- Hardware RT reflections for off-screen surfaces (sky reflections in windows, vehicle paint reflecting non-visible scene).
- Hybrid composite: SSR result if available, RT fallback for missing pixels.

### 7.7 Materials and shaders

- **Slang shader language** (modern, generic, compiles to SPIR-V and Metal IR).
- Shader hot-reload via filesystem watcher; recompile + pipeline rebuild between frames.
- Material system: a material is a Slang fragment-shader entry point + parameter block. Materials registered as components (`PSYNDER_COMPONENT(MaterialRef) { MaterialId id; };`) so the ECS owns material assignment.
- **PBR** by default (metallic-roughness, normal, AO, emissive). Authored via glTF metallic-roughness convention.

### 7.8 Temporal upscaling

Vendor-provided upscalers integrated as optional renderer passes:

- **NVIDIA DLSS** (RTX 20+)
- **AMD FSR 3** (cross-vendor)
- **Intel XeSS** (Arc + cross-vendor fallback)
- **Apple MetalFX** (Apple Silicon)

Selected automatically at startup based on detected GPU; user-overridable via console var `r_upscaler`. All SDKs are vendor-provided and integrate via thin wrappers under `psy-render-gx/upscale/`.

### 7.9 Variable rate shading

VRS on supported HW (RTX 20+, RDNA2+, Intel Arc). Lower-rate shading at screen edges + behind motion blur. ~10–20% perf win at minimal visual cost.

### 7.10 Performance budget (1440p60 internal, mid-range GPU baseline)

Target hardware: **RTX 4060 / RX 7600 / M3 Pro**. 16.6 ms frame budget.

| Stage | Budget (ms) |
|---|---|
| CPU extract + upload | 1.5 |
| GPU cull (compute) | 0.5 |
| Depth pre-pass + HiZ | 1.0 |
| Light cluster build | 0.3 |
| Opaque forward+ | 5.0 |
| RT shadows (or CSM) | 2.0 |
| DDGI update | 0.5 |
| SSR + RT reflections | 1.5 |
| Transparent pass | 1.0 |
| Post + upscale | 2.0 |
| UI + present | 0.5 |
| Slack | 0.86 |
| **Total** | **16.66 (60 FPS)** |

Lower-end target: 1080p60 on **RTX 3060 / RX 6600 / M2** with RT shadows off (CSM fallback) and FSR balanced upscale.

Competitive target (for the CS-tier mode): **1080p144** on the same mid-range HW, with reduced settings (no DDGI, CSM shadows, no RT reflections) — competitive players prefer perf over visuals.

### 7.11 Internal resolution model

Same philosophy as Psynder (see `DESIGN.md` §7.9 — fixed internal resolution, window resize scales the present): **internal render resolution is fixed**, window resize blits at scale. Unlike Psynder, GX *also* supports **dynamic resolution scaling** as an opt-in mode for competitive perf — `r_dynamic_res` console var. Default off (matches Psynder's stance); on means the renderer drops internal res to maintain target frame time.

---

## 8. Lighting — hardware-accelerated hybrid

Already described inline in §7 (RT shadows §7.4, DDGI §7.5, hybrid reflections §7.6). Summary:

- **Direct lighting:** forward+ clustered, supports ~256 dynamic lights.
- **Sun shadows:** hardware RT (with CSM fallback).
- **Dynamic light shadows:** hardware RT.
- **Indirect / GI:** DDGI probe grid, per-frame RT update on capable HW (or baked static fallback).
- **Reflections:** hybrid SSR + RT.
- **Volumetric fog:** froxel grid (compute), in-scattering integration. Same algorithm as Psynder (`DESIGN.md` §8.4) but evaluated on GPU.
- **Atmospheric scattering:** sky shader (Hillaire 2020 analytic), runtime-evaluated.
- **Static lightmaps:** baked offline via `lm_bake`-equivalent for non-DDGI static lighting, sampled in shader as a fallback.

---

## 9. World representation

Simpler than Psynder because the scope is tighter (no planet-scale, no streaming, smaller cap).

### 9.1 Map scale and format

- Max **2 km² (~1.4 km × 1.4 km)** per map.
- Whole map loads at level start; no mid-game streaming.
- Chunked LOD for distant terrain detail (CDLOD, polygon mesh — there's no software-renderer-style raymarcher option in GX because GPU rasterization is what GPUs do).
- Float32 world coords with per-frame render-relative origin re-centering. No double-precision needed.

### 9.2 Indoor — same BSP+PVS as Psynder

Same `psy-asset/bsp` module as Psynder (see `DESIGN.md` §9.1). BSP for indoor brush geometry, PVS for cell-based visibility culling. The BSP is compiled offline by `lm_qbsp`; the in-engine editor (§10.8) authors brushes via CSG primitives. Indoor lighting baked offline via `lm_bake`; dynamic lights composite on top via forward+.

### 9.3 Outdoor — heightmap + props

Polygon CDLOD heightmap terrain (no raymarcher backend in GX — see ADR-GX-008). Heightmap chunked at 64×64, 16-bit heights, 4-weight splatmap for materials. Material shading via the standard PBR pipeline. Vegetation / scatter: instanced billboards and meshes seeded from density maps, deterministic for multiplayer sync.

### 9.4 Unified scene graph

Same as Psynder (`DESIGN.md` §9.3). One transform hierarchy, partitions for `WorldStatic`, `WorldDynamic`, `Effects`, `UI`.

### 9.5 Spatial query routing

Same hybrid (`DESIGN.md` §9.4) — BVH for dynamic actors and RT, BSP+PVS for indoor visibility, hashed grid for nearest-neighbor, SAP for physics broadphase. The RT acceleration structure (TLAS + BLAS) is *also* a BVH and is built / maintained by the renderer alongside the gameplay BVH; they share the underlying SAH builder but are distinct trees.

---

## 10. Subsystems

### 10.1 Physics — Jolt + custom destruction + custom vehicles (`psy-phys`)

**Backbone: Jolt Physics** (MIT-licensed, vendored under `third_party/jolt/`). Modern C++17 codebase, multi-threaded, designed for game perf. Used by Horizon Forbidden West and others.

We chose Jolt over PhysX 5 + Blast after evaluation:
- MIT throughout matches our licensing story (vs BSD).
- Smaller dep, faster build, no CUDA toolchain.
- Cleaner integration with our DOTS / job system.
- PxVehicle and Blast are excellent but overkill at our 2 km² / 64-player scale; we cap vehicle complexity and destruction at levels Jolt + custom code handles well.
- Escape hatch: physics interface is abstracted; if BF-scale destruction or AAA-tier vehicles demand Blast / PhysX later, we integrate as a v2 module.

**Module structure:**

```
psy-phys/
├── core/                    ← Jolt wrapper, fixed 120 Hz tick, deterministic
├── vehicle/                 ← raycast suspension, Pacejka-lite tires, drivetrain (cars/jeeps/tanks)
├── destruction/             ← chunk-based pre-fracture, structural-integrity graph
├── character/               ← capsule-based FPS controller, crouch/prone/lean/ladder/water
└── debug/                   ← visualizer (renders to in-viewport ImGui overlay)
```

**Custom destruction (`psy-phys/destruction/`):**

- **Pre-fractured chunks** authored offline (artists fracture in Houdini / Blender / dedicated tools). Engine consumes the result as a hierarchy of rigid bodies + breakable joints.
- **Structural integrity graph:** each chunk has neighbors with joint-strength values. Stress propagates outward from impact; joints break when force exceeds threshold.
- **Cascade simulation:** for a typical "shoot a wall," 10–50 chunks affected; O(chunks-affected) work, runs in the physics tick.
- **Debris pool:** small chunks fade after time / distance; large chunks become permanent rigid bodies.
- **Authoring tool:** React editor panel for joint strengths, load-bearing chunk marking, fracture-seed authoring.

Estimated: 4–6 weeks of focused dev for a working v1 destruction system at our scale.

**Custom vehicle (`psy-phys/vehicle/`):**

- **Suspension:** raycast or sphere-cast at corner positions (4 for cars, 2 for motorcycles, N for tracked vehicles).
- **Tires:** Pacejka-lite combined-slip model.
- **Drivetrain:** engine torque curve → clutch → gearbox → diff → wheels.
- **Aero:** drag + downforce.
- **Damage:** scalar per panel + per-component health (engine, suspension, brakes).
- **Tracked vehicles:** per-track friction strips for tanks / APCs.
- **Helicopter/jet movers:** kinematic only; rigid body driven by script / animation.

Estimated: 6–8 weeks for vehicle module v1.

**Physics parallelism:** same model as Psynder (`DESIGN.md` §10.1) — parallel broadphase (Jolt's SAP), per-island parallel solver, all on the shared job pool.

**Determinism:** Jolt is deterministic across platforms when configured correctly (`-fno-fast-math` in physics TUs, FP rounding pinned). Required for lockstep multiplayer features and demo replay.

### 10.2 Audio — `psy-audio` (shared with Psynder, extended for voice)

Same CPU mixer architecture as Psynder (`DESIGN.md` §10.2): 32-channel software mixer, HRTF for first-person, FFT convolution reverb for indoor, FDN reverb for outdoor.

**GX additions:**

- **Voice chat mixing** — see §10.9.
- **Sound occlusion via raycasting** — when GPU RT is available, audio occlusion uses the same TLAS as the renderer (single ray per voice per frame is essentially free given the hardware is already traversing the tree for shadows).
- **Distance-based simulation LOD** — distant sounds processed at lower update rates.

Backends: WASAPI / PipeWire+ALSA / CoreAudio — unchanged from Psynder.

### 10.3 Input — `psy-platform/input`

- Keyboard + mouse + gamepad (XInput / evdev / GameController.framework).
- **Raw mouse input** mandatory for competitive FPS — bypasses OS acceleration, sub-frame polling.
- **Sub-tick input** (CS2-style) — input timestamps captured at sub-tick precision; server can interpolate aim direction within a tick.
- Force-feedback wheels for racing, controller rumble for FPS.

### 10.4 Networking — `psy-net-gx` (extends `psy-net`)

This is where Psynder-GX must not skimp. Competitive FPS netcode is the engine's reputation.

**Architecture: server-authoritative, no client trust.**

- **Dedicated server build target** (headless Linux, no rendering / audio / editor) for hosted competitive matches.
- **Listen-server option** for casual play (client also hosts).
- **Tick rate:** 64-tick default for casual, 128-tick competitive. Configurable per server.
- **Client-side prediction + reconciliation** (Quake/Source-derived) for local player movement and weapons.
- **Lag compensation** — server rewinds world state to the client's view-time when validating hitscans. Standard rewind window: 200 ms.
- **Snapshot interpolation** — clients display non-self entities with a 100–150 ms interp buffer for smoothness.
- **Reliable + unreliable UDP channels** with sequence numbers, sliding window, selective acks.
- **Snapshot delta compression** — XOR-style delta against the last acked baseline.
- **Demo / replay recording** — `.psydem` format. Records all inputs + snapshots. Replays deterministically. Supports fast-forward, rewind, free-camera.
- **Area-of-interest filtering** — *optional in v1*. At 64 players we can ship broadcast-all initially. AoI lands in v1.x as a perf optimization for the 64-player BF-light maps. Cell-based grid: each entity claims a cell; clients only receive entities in cells overlapping their view frustum + audio range.
- **Anti-cheat hooks** — designed for later EAC / BattlEye / Vanguard integration. Server validates all inputs against physics; clients can't cheat themselves into a position. v1 ships with the architecture, not the SDK integration.

**Sub-tick aim handling (CS2-style):**

When a player fires, the client samples their aim direction at sub-tick precision (e.g., a 16-bit fraction-of-tick). The server uses this fraction during lag compensation to construct the exact aim ray, not a tick-quantized one. This is what makes CS2's hitreg feel tighter than CS:GO's.

**Demo format:**

- Header: server version, map id, player roster, tick rate.
- Frame entries: tick number + delta-compressed snapshot.
- Input entries: per-tick player inputs for replay reconstruction.
- Indexable: seek to any frame in O(log n) via a TOC at the end of the file.
- Plays back deterministically because the engine is deterministic.

### 10.5 Scripting — same as Psynder

Lua 5.4, DOTS-for-users contract, live REPL bound to the editor (§10.8). Shared `psy-script` module.

### 10.6 UI — same three-surface hybrid as Psynder

- **React + Chrome editor panels** (out-of-process, multi-monitor docking) — see Psynder `DESIGN.md` §10.6.
- **Immediate-mode in-viewport overlays** (perf graphs, hitbox viz, manipulator gizmos) — minimal, in-engine.
- **RmlUi player HUDs + menus** — HTML/CSS subset, designer-authored, hot-reload via asset VFS.

In GX, the RmlUi `RenderInterface` is GPU-accelerated — triangles go through the same forward+ pipeline as everything else, with a UI-specific pipeline state. Fonts via FreeType (same as Psynder), glyph atlas as a GPU texture.

### 10.7 Asset pipeline — `psy-asset-gx` extends `psy-asset`

Base from Psynder (`DESIGN.md` §10.7), plus GPU-specific:

- **Texture cooker** outputs **BC1 / BC3 / BC7 / ASTC** mipmap chains. Compression via `bc7enc`, `ispc_texcomp`, or similar. Cook-time tuneable quality.
- **Mesh cooker** outputs vertex-format variants per pipeline (positions-only for depth pass, full for opaque pass) and pre-computes mesh-shader meshlets (where applicable).
- **Shader cooker** compiles Slang sources to SPIR-V (for Vulkan) + Metal IR (for Apple Silicon native). Pipeline state objects (`VkPipeline` / `MTLRenderPipelineState`) precompiled at build, cached on first run.
- **Hot reload** for textures, meshes, shaders. Same VFS watcher as Psynder.

### 10.8 Editor — same as Psynder

React + Chrome panels, WebSocket IPC, in-engine editor mode toggle (`~` / `F2`). Same `engine/editor/` layout (`core/`, `ipc/`, `web/`). Same Gmod-style sandbox + level-authoring features. See Psynder `DESIGN.md` §10.8.

**GX-specific editor additions:**

- **Material editor** — Slang shader authoring with hot-reload preview.
- **Destruction authoring** — pre-fracture mesh, mark load-bearing chunks, tune joint strengths, preview cascade.
- **Lightmap / probe baker UI** — trigger `lm_bake` runs, preview probe density, debug-visualize DDGI in the editor.
- **Network tools** — replay viewer (loads `.psydem`), tick-rate visualizer, snapshot diff viewer.

### 10.9 Voice chat — `psy-net-gx/voice`

**Decision: ship in v1, built in, Opus-based.**

- **Codec: Opus** (BSD-licensed reference impl, vendored).
- **Capture: platform-native** — WASAPI loopback / PulseAudio source / AudioUnit on macOS.
- **Transport:** the same UDP channel as gameplay netcode; voice packets get their own unreliable channel with sequence numbers but no retransmission.
- **Mixing:** server-side mixing per recipient (team channel + proximity-falloff for positional voice). Each client receives one pre-mixed voice stream per tick, sub-2 KB.
- **Positional:** voice gets the same HRTF and reverb treatment as in-game audio (`psy-audio`). Speak from in-game player position.
- **Push-to-talk + open-mic + voice-activation-detection (VAD).**
- **Mute lists** synced via reliable channel.

Estimated: 6–8 weeks for v1 voice chat. Genuinely useful as a gameplay feature (audio direction-finding for footsteps + voice).

---

## 11. Platform layer — `psy-platform-gx`

### 11.1 Windows

- Win32 directly, no SDL dep.
- **Vulkan surface** via `vkCreateWin32SurfaceKHR`. No D3D12.
- Audio: WASAPI (shared with Psynder).
- Raw mouse input via `RegisterRawInputDevices`.

### 11.2 Linux

- **Wayland primary** (xdg-shell + Vulkan surface via `vkCreateWaylandSurfaceKHR`).
- **X11 fallback** via `vkCreateXlibSurfaceKHR` / `vkCreateXcbSurfaceKHR`.
- Audio: PipeWire / ALSA.
- Raw input via evdev.
- Headless mode (dedicated server) doesn't initialize any windowing.

### 11.3 macOS Apple Silicon

- AppKit + **native Metal** (`MTKView` / `CAMetalLayer`). No MoltenVK.
- The `psy-gpu` abstraction has a Metal backend matching the Vulkan backend feature-for-feature.
- Unified memory architecture exploited (§4.2).
- Audio: CoreAudio.
- Raw input via `GCController` for gamepads, IOKit for raw mouse.

### 11.4 Dedicated server build

- Linux x86-64 only initially (extend later if needed).
- Headless: no graphics, no audio, no editor, no Vulkan.
- Build flag: `PSYNDER_DEDICATED_SERVER=1` strips all rendering/audio/editor code.
- Smaller binary, faster startup, lower memory footprint.

### 11.5 Build matrix (CI)

| OS | Arch | Compiler | Graphics | Build flags |
|---|---|---|---|---|
| Windows 11 | x86-64 | Clang 17, MSVC | Vulkan | PSYNDER_EDITOR=1 |
| Ubuntu 24.04 | x86-64 | Clang 17, GCC 13 | Vulkan | PSYNDER_EDITOR=1 |
| Ubuntu 24.04 | x86-64 | Clang 17 | None | PSYNDER_DEDICATED_SERVER=1 |
| macOS 14+ | arm64 | Apple Clang | Metal | PSYNDER_EDITOR=1 |

---

## 12. Repository layout

```
psynder-gx/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── README.md
├── DESIGN-PSYNDER-GX.md      ← this doc
├── LICENSE-MIT
├── LICENSE-APACHE
├── .clang-format
├── .clang-tidy
├── .github/workflows/        ← CI (3 OSes + dedicated server)
├── docs/
│   ├── 01-getting-started.md
│   ├── 02-rendering-gx.md
│   ├── 03-netcode.md
│   ├── 04-physics-and-destruction.md
│   └── adr/                  ← GX-specific architecture decision records
├── engine/
│   ├── render-gx/
│   │   ├── pipeline/         ← forward+, depth pre-pass, RT shadow, DDGI, SSR
│   │   ├── upscale/          ← DLSS, FSR, XeSS, MetalFX wrappers
│   │   └── post/             ← bloom, tonemap
│   ├── gpu/                  ← psy-gpu: Vulkan + Metal abstraction
│   │   ├── vk/
│   │   ├── mtl/
│   │   ├── heap/             ← device-memory allocators
│   │   └── cmd/              ← command-buffer recording
│   ├── shader/               ← Slang compiler, hot-reload
│   ├── net-gx/               ← psy-net-gx: 128-tick, lag comp, demo, voice
│   │   ├── server/           ← dedicated-server-only code
│   │   ├── client/
│   │   ├── voice/            ← Opus, mixing, VAD
│   │   └── replay/           ← .psydem reader/writer
│   ├── platform-gx/          ← Vulkan/Metal surfaces, raw input
│   │   ├── win32/
│   │   ├── linux/
│   │   └── macos/
│   └── editor/               ← shared with Psynder via psy-editor module
│       ├── core/
│       ├── ipc/
│       └── web/
├── tools/
│   ├── lm_cook_gx/           ← GPU-aware asset cooker (BC/ASTC textures, mesh shader meshlets)
│   ├── lm_bake_gx/           ← GPU-accelerated probe + lightmap baker
│   ├── lm_qbsp/              ← BSP compiler (shared with Psynder)
│   └── lm_pak/               ← archive tool (shared with Psynder)
├── samples/
│   ├── 01_triangle/
│   ├── 02_pbr_sphere/
│   ├── 03_quake_room_gx/     ← Quake-room demo via GPU path
│   ├── 04_cs_arena/          ← 10v10 CS-scale competitive map demo
│   ├── 05_dest_sandbox/      ← chunk destruction sandbox
│   ├── 06_vehicle_test/      ← vehicle drive demo
│   └── 07_bflight_map/       ← BF-light 32v32 map with vehicles + destruction
├── third_party/              ← Jolt, Opus, FreeType, RmlUi, Slang, Vulkan SDK, vendored upscaler SDKs
└── tests/
    ├── unit/
    ├── golden/               ← golden-image renderer tests (compared against reference)
    ├── netcode/              ← lag-comp and replay determinism tests
    └── bench/

# Shared modules (separate repos, consumed via vcpkg):
#   psy-core / psy-scene / psy-phys / psy-audio / psy-net / psy-script
#   psy-asset / psy-editor / psy-ui-rml / psy-platform
```

---

## 13. Milestone roadmap

A demo at every milestone, mirroring Psynder's discipline.

### M0 — Bring-up (weeks 1–4)

- Repo, CI, three-OS Vulkan + Metal "clear color" build.
- `psy-core` + `psy-math` + `psy-simd` consumed from shared modules.
- `psy-gpu` skeleton: Vulkan instance + device + swapchain on Win/Linux; Metal device + view on macOS.
- **Demo:** animated clear color via the GPU API on all three OSes (sample 01).

### M1 — First textured triangle (weeks 5–8)

- `psy-gpu/cmd` command-buffer recording, basic resource creation.
- Slang shader compiler integration, SPIR-V + Metal IR pipeline.
- Vertex / index buffer upload, texture sampling.
- **Demo:** rotating textured triangle (sample 01 extended).

### M2 — Forward+ + PBR + meshes (weeks 9–14)

- Forward+ light clustering compute pass.
- PBR fragment shader, glTF mesh loading.
- Depth pre-pass + HiZ.
- Basic camera + scene navigation.
- **Demo:** PBR sphere with point lights (sample 02).

### M3 — BSP indoor + editor v0 (weeks 15–22)

- Shared `psy-asset/bsp` integration; load Quake-style BSP into GX renderer.
- Lightmap atlas as texture, sampled in PBR shader.
- **Editor IPC bring-up** (same WebSocket + React stack as Psynder).
- Inspector panel + immediate-mode in-viewport overlays.
- **Demo:** walk through a small Quake-style room with PBR materials (sample 03).

### M4 — Outdoor + heightmap + Jolt physics (weeks 23–30)

- Polygon CDLOD heightmap terrain.
- `psy-phys` integration: Jolt physics, rigid bodies, character controller.
- First playable: walk a soldier around an outdoor map.
- **Demo:** soldier walks across a 1 km² test map (sample 04 wip).

### M5 — Hardware ray-traced shadows + DDGI (weeks 31–40)

- Hardware RT pipeline: TLAS / BLAS build, refit, packet tracing wrapped in `psy::gpu::RayTracing`.
- RT shadow pass; CSM fallback for non-RT HW.
- DDGI probe grid + per-frame update.
- Hybrid SSR + RT reflections.
- **Demo:** indoor + outdoor scene with full RT lighting (sample 04).

### M6 — Networking core + CS-scale playable (weeks 41–52)

- `psy-net-gx`: server-auth architecture, 64-tick + 128-tick capable.
- Client-side prediction + reconciliation.
- Lag compensation with rewind hit-tracing.
- Snapshot delta compression.
- Demo / replay (`.psydem`) recording + playback.
- Dedicated-server build target.
- **Demo:** 10v10 CS-scale playable match on a small map, two physical machines connected (sample 04 — first playable competitive demo).

### M7 — Vehicles + custom destruction (weeks 53–66)

- `psy-phys/vehicle`: raycast suspension, Pacejka tires, drivetrain. Drivable car + jeep + light tank.
- `psy-phys/destruction`: chunk-based pre-fracture, structural-integrity graph, cascade simulation.
- Destruction authoring in the React editor.
- **Demo:** drive a tank across a map and destroy a building (sample 05 + 06).

### M8 — Voice chat + BF-light scale + AoI (weeks 67–78)

- `psy-net-gx/voice`: Opus codec, server-side mixing, positional + push-to-talk.
- Area-of-interest filtering for 32v32 / 64-player scale.
- Scale-test on the BF-light 2 km² map.
- **Demo:** 32v32 match on a 2 km² map with vehicles, destruction, and positional voice (sample 07 — full BF-light demo).

### M9 — Polish & 1.0 (weeks 79+)

- Vendor upscaler integration: DLSS / FSR / XeSS / MetalFX.
- VRS where available.
- Mesh shaders where available.
- Performance tuning across the matrix.
- Editor polish, documentation, contributor guide.
- Public release.

Total: ~24 months for a small team, ~36 months solo. Larger than Psynder by ~33% — the renderer and netcode chapters are simply more work.

---

## 14. Coding standards

Inherited from Psynder (`DESIGN.md` §14). Notable GX-specific additions:

- **GPU resources** are owned by `psy::gpu::Handle<T>` (intrusive refcount with frames-in-flight tracking). Banned everywhere outside `engine/gpu/`: raw `VkBuffer` / `MTLBuffer` ownership.
- **No mid-frame GPU allocations.** Allocator lint catches `vkAllocateMemory` / `[MTLDevice newBuffer*]` outside `engine/gpu/heap/`.
- **Shader hot-reload** must preserve currently-running frames; pipeline rebuild happens between frames.
- **Determinism in physics + netcode TUs**: `-fno-fast-math`, integer accumulators where possible. Same rule as Psynder. Lockstep replay determinism is tested in CI.

---

## 15. Contribution model

Same as Psynder (`DESIGN.md` §15). Squash-merge to `main`, ADRs for cross-system changes, Contributor Covenant 2.1, good-first-issues seeded per milestone.

**Family-wide contribution policy:**

- A change to a shared module (`psy-core`, `psy-scene`, etc.) must pass CI on both Psynder and Psynder-GX.
- A change to a GX-specific module only needs GX CI.
- ADRs that affect shared modules need cross-engine acknowledgement.

---

## 16. Risks & ADR log

| Risk | Severity | Mitigation |
|---|---|---|
| Custom destruction insufficient at BF-light scale | Medium | 2 km² / 64-player target keeps complexity manageable; Blast escape hatch in v2 |
| Custom vehicle feel underwhelming vs PxVehicle | Medium | Lean on shared `psy-phys/vehicle` design from Psynder; iterate via real playtest |
| 128-tick netcode harder than expected | High | Source 2 / Quake patterns are well-documented; ship 64-tick first, scale up |
| Anti-cheat absent in v1 | Known | Architecture is server-authoritative; SDK integration possible later. Document loudly. |
| Vulkan-on-macOS path missing for testing (no MoltenVK) | Low | Native Metal backend is first-class; macOS users get the better path |
| GPU driver bugs on Linux (especially Wayland) | Medium | Test matrix includes both Wayland and X11; Vulkan validation layers in dev builds |
| Hardware RT only on recent GPUs | Acceptable | CSM + baked GI fallback runs on any Vulkan-capable HW |
| Mesh shaders not universally available | Low | Legacy vertex/index pipeline is the default; mesh shaders opt-in |
| Voice chat latency / quality | Medium | Opus is mature; positional voice via shared `psy-audio` HRTF; tune over time |

**ADR log (decisions and open questions — each gets a file under `docs/adr/` as settled):**

- **ADR-GX-001:** ✅ **Decided.** Vulkan primary on Windows + Linux. Native Metal on macOS Apple Silicon v1.0. No D3D12. No MoltenVK as primary. Skip D3D12 entirely for codebase simplicity; Vulkan on modern Windows drivers is fine. See §11.
- **ADR-GX-002:** ✅ **Decided.** Jolt Physics (MIT) as the physics core. Custom destruction module (chunk-based, structural-integrity graph). Custom vehicle module (raycast suspension, Pacejka tires, drivetrain). PhysX 5 + Blast evaluated and rejected: heavier dep, NVIDIA-tied tooling, our 2 km² / 64-player scale doesn't require AAA-tier destruction or vehicle machinery. Escape hatch: physics interface abstracted; if BF-scale demands Blast / PhysX later, integrate as v2. See §10.1.
- **ADR-GX-003:** ✅ **Decided.** Forward+ shading everywhere (deferred rejected). Reasons: composes with hardware RT, MSAA-friendly, lower bandwidth at 1080–1440p, single shader path for opaque + transparent. See §7.3.
- **ADR-GX-004:** ✅ **Decided.** Hardware RT shadows + DDGI for GI on capable HW (RTX 20+/RDNA2+/M3+); CSM + baked-probe fallback on non-RT HW. Unified RT API (`psy::gpu::RayTracing`) wraps Vulkan + Metal RT pipelines. See §7.4–§7.5.
- **ADR-GX-005:** ✅ **Decided.** Server-authoritative architecture from day 1. 128-tick capable, 64-tick default for v1, 128-tick standard for competitive. Client-side prediction + reconciliation + lag compensation (Source 2 / Quake-derived). Demo / replay format (`.psydem`) baked in. See §10.4.
- **ADR-GX-006:** ✅ **Decided.** Area-of-interest filtering is a **v1.x feature, not v1.0**. At 64 players we can ship broadcast-everything netcode initially and add AoI as a perf optimization once gameplay is stable. Decision revisited at M6.
- **ADR-GX-007:** ✅ **Decided.** Voice chat shipped in v1.0. Opus codec (BSD), server-side mixing per recipient, positional voice via shared `psy-audio` HRTF, push-to-talk + open-mic + VAD. Same UDP transport as gameplay netcode. See §10.9.
- **ADR-GX-008:** ✅ **Decided.** No Nanite-style virtualized geometry; no Lumen-tier full GI; no voxel destruction; no cloth / fluids / soft bodies; no full flight sim. All explicitly out of v1 scope. See §1 non-goals.
- **ADR-GX-009:** ✅ **Decided.** Map cap **2 km² (~1.4 km × 1.4 km)** per map, whole-map load at level start, no mid-game streaming. Chunked LOD for terrain detail at distance. Float32 world coords with per-frame render-relative origin re-centering. See §9.1.
- **ADR-GX-010:** ✅ **Decided.** CS-scale (10v10, small maps) first; scale up to BF-light (32v32, 2 km² with vehicles + destruction) in later milestones. M6 is the first competitive playable; M8 hits BF-light scale. See §13.
- **ADR-GX-011:** ✅ **Decided.** Dedicated server is a separate build target (`PSYNDER_DEDICATED_SERVER=1`), strips rendering / audio / editor for smaller, headless Linux binary. See §11.4.
- **ADR-GX-012:** ✅ **Decided.** Player UI via shared `psy-ui-rml` module (RmlUi HTML/CSS subset, GPU-accelerated RenderInterface in GX). Editor UI via shared `psy-editor` module (React + Chrome + WebSocket IPC). In-viewport overlays via minimal in-engine immediate-mode UI. Same three-surface split as Psynder.
- **ADR-GX-013:** ✅ **Decided.** No anti-cheat shipped in v1. Architecture is server-authoritative and designed for later EAC / BattlEye / Vanguard SDK integration. Documentation makes this explicit so contributors and prospective licensees can plan accordingly.
- **ADR-GX-014:** ✅ **Decided.** Dynamic resolution scaling supported as opt-in (`r_dynamic_res`), default off. Fixed internal render resolution is the default (matches Psynder's stance); competitive players who want stable frame time can opt in.
- **ADR-GX-015:** ✅ **Decided.** Slang as the shader language (modern, generic, compiles to SPIR-V + Metal IR). Falls back to glslc + spirv-cross if Slang adoption stalls. Materials are Slang fragment-shader entry points + parameter blocks registered as ECS components.
- **ADR-GX-016:** ✅ **Decided.** Shared module strategy (see §5): foundation modules (`psy-core`, `psy-scene`, `psy-phys`, `psy-audio`, `psy-net`, `psy-script`, `psy-asset`, `psy-editor`, `psy-ui-rml`, `psy-platform`) live in their own git repos, versioned independently, consumed by both Psynder and Psynder-GX as pinned dependencies. Renderer + GPU + shader + GX-specific netcode tuning + GPU platform shims are GX-specific.

---

## 17. Prior art & inspiration

- Michael Abrash, **Graphics Programming Black Book** (for the math heritage).
- **Source 2** netcode papers (Valve developer wiki, Glenn Fiedler articles) — the canonical reference for competitive FPS networking.
- **Quake source releases** (GPLv2) — under `third_party/quake-refs/` for study.
- **id Tech papers** — virtualized texture / megatexture for inspiration (not implemented).
- **Counter-Strike 2 sub-tick papers** (Valve technical posts).
- **DDGI paper** (Majercik et al., "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields", 2019).
- **NVIDIA RTX whitepapers** for hardware ray-tracing techniques.
- **Frostbite presentations** (GDC, SIGGRAPH) — destruction system architecture reference.
- **Jolt Physics documentation** + Horizon Forbidden West GDC talk.
- **Hillaire 2020** — analytic atmospheric scattering.
- **Battlefield 3 / 4 retrospectives** — 64-player FPS architecture.
- **Squad / Insurgency post-mortems** — tactical-scale FPS design.

---

## 18. What "done" looks like

When Psynder-GX hits 1.0, the sizzle reel runs:

1. A 5v5 CS-scale match, mid-fight: clean hitreg, sub-tick aim, lag-comp visualization in spectator mode, demo recording running silently.
2. Cut to a 32v32 BF-light match on a 2 km² map: vehicles maneuvering, a tank shell punching through a wall (chunk destruction cascading), squad on positional voice chatter, hardware RT shadows from the tank's headlight onto a wet road.
3. Cut to the in-engine editor: a developer drags the Inspector panel to a second monitor, fractures a building into chunks with a slider, shoots it from a third panel showing live profiler stats.
4. End on three terminals side-by-side — RTX 4070 Windows machine, Intel Arc Linux box, M3 Pro MacBook — all running the same Vulkan / Metal binary, all at 144 FPS at 1440p, all on a dedicated Linux server fielding 32v32.

That's the bar.

---

## 19. Implementation handoff

This doc is complete enough to start building from. If you're an implementer (human or agent) picking this up cold, here's how to use it.

### 19.1 What to read first

1. **Read `DESIGN.md` (Psynder CPU engine) first** — Psynder-GX inherits its DOTS contract, memory hierarchy, ECS, editor IPC, scripting, and audio mixer. ~70% of the engine is shared.
2. **Then read this doc's §1 (Vision)** for the GX-specific targets and non-goals.
3. **Then §5 (Relationship to Psynder)** to internalize what's shared vs GX-specific.
4. **Then §7 (Renderer)** — the largest fresh chapter, where most of GX's work lives.
5. **Then §10.4 (Networking)** — the second-largest fresh chapter, critical for the competitive FPS identity.
6. **Then §16 ADR log** — 16 decisions, all called.
7. **Then §13 Milestone roadmap** — the build order.

### 19.2 What to start with — M0 bring-up

1. Create the repo scaffold per the layout in §12.
2. CMakeLists.txt + CMakePresets for the four CI matrix entries (Win/Linux/macOS + dedicated server).
3. Vendor or vcpkg the shared modules: `psy-core`, `psy-scene` minimum for M0. Others as their dependencies become needed.
4. Vendor third-party deps: Vulkan SDK (LunarG), Jolt Physics, Opus, FreeType, RmlUi, Slang.
5. `psy-gpu` skeleton: Vulkan instance + device + swapchain on Win/Linux; native Metal device + view on macOS.
6. Open a window on each platform, present an animated clear color.

When sample 01 renders an animated clear color on all three OSes from one binary, M0 is done.

### 19.3 What to *not* build first

- Hardware RT or DDGI. Defer to M5.
- Netcode. Defer to M6 — let the rendering and physics solidify first.
- Vehicles or destruction. Defer to M7.
- Voice chat. Defer to M8.
- Anti-cheat. Not in v1 at all (ADR-GX-013).
- Mesh shaders. Implement the legacy vertex path first; mesh shaders are a perf optimization layered on top.

### 19.4 Hard rules

- No D3D12 backend. Vulkan only on Windows.
- No MoltenVK. Native Metal on macOS.
- No `vkAllocateMemory` / `[MTLDevice newBuffer*]` in the frame loop.
- No `std::shared_ptr` outside `engine/gpu/` resource handles.
- All hot paths follow DOTS rules (see Psynder `DESIGN.md` §3).
- Determinism flags (`-fno-fast-math` in physics + netcode TUs) — required for lockstep replay.
- Per-tile / per-pass perf regression > 2% must justify in PR or fail CI.

### 19.5 Order of subsystem dependencies

```
psy-core → psy-math → psy-simd → psy-jobs → psy-asset → psy-scene → psy-gpu → psy-shader →
psy-render-gx → psy-audio → psy-script → psy-phys → psy-net-gx → psy-editor → game
                                                                  ↑
                                                              psy-ui-rml
```

Bring up bottom-up. Stub upper subsystems with canned returns until their dependencies land.

### 19.6 Where this doc and Psynder's doc disagree

This doc explicitly diverges from Psynder in these areas:

- **Renderer:** GPU-driven Vulkan / Metal vs CPU software (§7).
- **Memory:** adds GPU heap allocators on top of CPU side (§4).
- **Physics:** Jolt + custom destruction + custom vehicles vs psynder_phys custom (§10.1). *Note:* if you want to unify by switching Psynder to Jolt too, that's a coordinated change across both docs — open an issue.
- **World scale:** 2 km² cap vs Psynder's 16 km² — neither uses double-precision coords.
- **Netcode:** 64 players × 128 tick + lag comp + demos vs Psynder's 8-player lockstep racing + 16-player FPS.
- **Map streaming:** none in GX (whole map loads); chunked LOD only.
- **Anti-cheat hooks:** in GX architecture; not in Psynder.
- **Dynamic resolution scaling:** opt-in in GX; explicitly forbidden in Psynder.
- **Player UI:** RmlUi via GPU-accelerated RenderInterface in GX; via software rasterizer in Psynder.

Everywhere else, defer to Psynder's design — it's the canonical reference for the shared modules.

### 19.7 Open questions for implementation

The doc commits to *what* and *why*; some *how* details are intentionally not specified:

- Exact Vulkan-loading library (volk, custom).
- Exact Slang version pin (latest stable at integration time; submodule).
- Exact mesh-shader pipeline variant (V8 / V16 — pick on bench results).
- Exact descriptor strategy (descriptor indexing vs bindful — descriptor indexing on capable HW, bindful fallback).
- Whether to ship a Vulkan + Metal unified pipeline cache or separate.

These are micro-decisions; make them with judgment.

### 19.8 Dev environment prerequisites

- C++23-capable compiler (Clang ≥ 17 primary).
- CMake ≥ 3.28, Ninja, vcpkg.
- **Vulkan SDK** (LunarG) on Windows and Linux.
- **Xcode + Metal SDK** on macOS Apple Silicon.
- **Slang compiler** (or glslc + spirv-cross fallback).
- Node.js ≥ 20 + pnpm (for the editor's React panels under `engine/editor/web/`).
- Chrome or Chromium installed and on PATH (the engine launches it via `--app=` per editor panel).
- For dedicated-server builds: only the C++ toolchain is needed (no Vulkan, no Node, no Chrome).

For *shipping* a game built on Psynder-GX, the runtime dependencies are: Vulkan loader + ICD (or Metal on macOS), an audio device, an internet connection for multiplayer. No editor, no Node, no Chrome.

---

*— end of design v1.0 (handoff)*
