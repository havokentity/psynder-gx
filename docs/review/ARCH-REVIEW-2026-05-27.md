# Psynder-GX — Architecture & Dependency Review

- **Date:** 2026-05-27
- **Scope:** Holistic, read-only review of the uncommitted working tree on branch
  `codex/miniwave-dots-crate` (level with `main` — the entire body of new work is
  uncommitted). Two AI-agent-authored bodies of work, never holistically reviewed.
- **Method:** Direct reading of build system, ECS core, scripting/codegen spine, the
  playable sample, netcode, and dependency wiring; two assisting agents for the
  hot-path hygiene scan and the dependency-acquisition trace. Evidence is cited as
  `file:line`. No source was modified.

---

## 1. Executive summary

The foundation is **higher quality than the briefing implies** in the disciplined
areas (no exceptions/RTTI/`shared_ptr`/virtual in hot paths, a genuinely
cache-coherent archetype ECS, and a Jolt determinism setup that is correct in every
detail). The risks are concentrated in three places: **(a) two parallel "scene"
worlds** that the upcoming scene-on-ECS migration sits directly on top of, **(b) a
dependency/attribution story that is incoherent and partly fictional**, and **(c) a
handful of latent traps** (unguarded structural mutation during iteration, call-order
component IDs) that are harmless today only because the features that would trigger
them don't exist yet — and the migration is about to build exactly those features.

### Top 5 risks

1. **The parallel-world divergence (Track A, A2 — HIGH).** The only playable sample
   (`samples/02_crate/main.cpp`) simulates on `physics/core/CharacterSpine` and renders
   a bespoke `ScenePrimitive` list — *not* the `scene::World` archetype ECS, and not
   the ECS render path (`render/pipeline/Extract.cpp`). The ECS-native gameplay slice
   (`samples/combat/Combat.h`) is wired to nothing but its own unit test. You have two
   scene representations, two renderers, and two physics paths. The scene-on-ECS layer
   must choose which survives before it is built.

2. **`vcpkg.json` is dead + `NOTICE` is fictional (Track B, B1/B2 — HIGH).** No preset
   sets a vcpkg toolchain, so the manifest is never consumed; 7 of its 16 entries are
   wired nowhere. `NOTICE` attributes a dozen vendored `third_party/` directories that
   **do not exist** (the deps are FetchContent/`find_package`/system-SDK), and
   metal-cpp is listed but **not used at all**. The license audit can't be trusted
   until `NOTICE` reflects the real dependency set.

3. **Determinism deps are not hash-pinned (Track B, B3 — HIGH).** The lockstep pillar
   requires identical physics across client/server/platforms. Jolt's *flags* are
   perfectly controlled, but Jolt itself is fetched by mutable git **tag** with
   `GIT_SHALLOW`, and Vulkan + `slangc` are resolved from whatever the host has. Only
   Lua is hash-pinned. A retagged upstream or a different dev box can silently change
   simulation output.

4. **Structural change during ECS iteration is unguarded (Track A, A1 — HIGH).**
   `query_chunks`/`for_each_chunk` hand out raw pointers into chunk memory;
   `World::add/remove/create/destroy` can reallocate the chunk vector and invalidate
   them. `scene::CommandBuffer` exists to defer this but nothing *enforces* its use —
   no debug re-entrancy trap. This is the #1 footgun the migration's system authors
   will hit.

5. **Call-order component IDs (Track A, A3 — latent, MEDIUM→HIGH on trigger).**
   Component IDs are assigned by static-init/first-use order and are not stable across
   builds/compilers. Currently harmless (no numeric ID is ever serialized), but the
   planned delta-netcode / save / golden-replay features will serialize state and break
   cross-binary the moment they exist.

### Go / no-go on building scene-on-ECS next

**Conditional GO — but three decisions must be made first, and two fixes must land
first.** The ECS core is sound enough to build on. Before the migration:

- **Decide (design):** which scene representation is authoritative (ECS vs
  `ScenePrimitive`/`CharacterSpine`); how the editor's string-keyed scene document maps
  onto ECS components; and what the stable, cross-binary identity of a component and an
  entity is on the wire/disk (see A3, A4).
- **Fix first (cheap, prevents rework):** add the debug structural-change guard (A1) and
  switch component IDs to a name-hash or explicit table (A3). Both are small and both
  are load-bearing for everything the migration adds.

Building the migration on top of the current call-order IDs + unguarded iteration +
the spine/ECS split would bake all three problems into the new layer.

---

## 2. Track A — Code / architecture findings

### A1 — Unguarded structural mutation during chunk iteration — **HIGH**
`World::query_chunks` iterates `for (Chunk& chunk : archetype.chunks)` and exposes raw
column base pointers into live chunk memory (`engine/scene/World.cpp:525-538`,
contract in `engine/scene/World.h:95-118`). Any `World::add/remove/create/destroy`
called *during* a `for_each_chunk` body can `allocate_chunk` →
`archetype.chunks.push_back` (`engine/scene/World.cpp:244`), reallocating the
`std::vector<Chunk>` and dangling both the `Chunk&` and the `columns[]` bases the
callback is using. `ADR-018` and `CommandBuffer.h:5-9` both *say* structural changes
must be deferred, but the API provides no enforcement.
**Why it matters:** silent UB, and exactly the mistake every system author makes once.
**Fix:** add an iteration-depth counter on `WorldState`; in debug, assert it is zero on
entry to `add/remove/create/destroy` (or trap with a clear message). Cheap, and it
converts a latent corruption into a loud test failure.

### A2 — The "parallel world": spine + primitive list vs. ECS — **HIGH**
The playable windowed sample runs its simulation on the self-described "smoke spine":
`PlayModeState` holds `character_spine::World*` / `character_spine::Character*`
(`samples/02_crate/main.cpp:198-199`), builds colliders from a
`std::vector<ScenePrimitive>` (`:190`, `add_play_collider_for_primitive` `:1027`), and
renders that primitive list through a bespoke `PrimitiveRenderResources`
(`:225-239`). `CharacterSpine` is explicitly *outside* the frozen physics contract
(`engine/physics/core/CharacterSpine.h:5-9`) and is used by **only** this sample.

Meanwhile `scene::World` is used by the sample **only** to hold editor-side crate
entities (`create_fallback_crate`/`create_script_crate`
`samples/02_crate/main.cpp:3110-3137`, read back at `:4521`), and the ECS-native
gameplay reference (`samples/combat/Combat.h` — proper `for_each_chunk` +
`raycast_nearest` + deferred command buffer, `:82-117`) is referenced by nothing but
its unit test. There is also a parallel **render** path: `render/pipeline/Extract.cpp`
+ `Pipeline.cpp` extract from the ECS, but the sample does not use them.

**Net:** three divergent representations of "the scene" (ECS / ScenePrimitive+spine /
the editor JSON document). The spine encodes real gameplay decisions (capsule
controller tuning, collider synthesis) that have no ECS equivalent yet.
**Recommended convergence path:** (1) declare the ECS authoritative; (2) port the
character controller to a real `physics/core` body fed by an ECS component, retiring
`CharacterSpine` to test-only; (3) make play-mode build colliders + render from the
ECS via the existing `Extract`/`Pipeline`, deleting the sample's bespoke primitive
renderer; (4) make the editor JSON document a serialization of ECS entities, not a
third parallel model. **Decide before building:** the ECS↔editor-document mapping and
entity/component wire identity (A3/A4) — the migration can't be correct without them.

### A3 — Component IDs assigned by call/static-init order — **MEDIUM (latent), HIGH when serialized**
`component_id<T>()` lazily registers on first use (`engine/scene/World.h:38-44`) and
`register_component` assigns `id = types.size()` (`engine/scene/World.cpp:378`). The
`PSYNDER_COMPONENT` macro triggers this via an `inline` variable whose cross-TU init
order is unspecified (`engine/scene/World.h:46-51`). So archetype column layout and
component IDs are **not stable across builds, compilers, or link order**.
**Currently latent:** no numeric `ComponentId` is serialized anywhere — the net
snapshot is a fixed schema (`engine/net/Snapshot.h:39-44`) and editor save keys
components by **string name** (`entity_has_component("PlayerStart")`,
`samples/02_crate/main.cpp:976`). Confirmed: `grep ComponentId` across
`engine/net engine/asset engine/editor` returns nothing.
**Why it becomes HIGH:** `ADR-018` plans delta-netcode, golden cross-platform replay,
and save/load — all serialize state, all will key on component identity.
**Fix before any binary/numeric serialization:** assign IDs by a stable hash of the
(fully-qualified) component name, or an explicit registration manifest, instead of
discovery order.

### A4 — Snapshot truncates the 64-bit Entity to `u32` — **MEDIUM**
`Entity` is a 64-bit handle (32-bit gen | 32-bit index, `engine/core/Types.h:44-50`),
but `SnapshotEntity::entity_id` is `u32` (`engine/net/Snapshot.h:27`) with a fixed
20-byte wire record (`:40-44`). The generation is dropped, so two entities sharing an
index across ticks alias on the wire. Define the wire identity explicitly (index-only
is fine *if* documented and if interpolation/AOI never need to disambiguate
generations) before snapshots carry gameplay state.

### A5 — CommandBuffer temp-handle bit overlaps the Entity generation field — **MEDIUM**
`CommandBuffer` tags temp entities with bit 63 (`engine/scene/CommandBuffer.h:93`),
which lives in `Entity`'s generation half. A real entity with `gen >= 2^31` would be
misread as a temp handle. Practically unreachable, but it silently couples the command
buffer to the handle bit-layout; if `Entity` packing ever changes (it just did,
8→32-bit gen) this breaks. Document the invariant or reserve the bit in `Types.h`.

### A6 — Duplicated CommandBuffer + render concepts — **MEDIUM**
Two `CommandBuffer` types exist: `scene::CommandBuffer`
(`engine/scene/CommandBuffer.h`) and `script::behavior::CommandBuffer`
(`engine/script/behavior/BehaviorSpine.h:43`, just a `std::vector<u32> destroy`). The
behavior one is a proving-spine toy, so the duplication is *currently* defensible, but
the real one already does what the spine needs — fold the spine onto it when the
front-ends land. Likewise the dual render path (A2).

### A7 — 4,701-line "sample" with a hand-rolled JSON parser — **MEDIUM**
`samples/02_crate/main.cpp` is 4,701 lines and is, in practice, the editor + player +
IPC host. It hand-rolls a JSON scanner (`parse_json_*`, `find_json_*`,
`patch_*_json`, `:388-985`) that does string-scanning with ad-hoc escape
handling and `strtof` slicing. This is fragile (no number/structure validation),
security-adjacent if it ever ingests untrusted scene files, and contradicts the file's
own "stay boring and load-bearing: one crate" header comment (`:6-8`). Extract the
editor/IPC/scene-document logic into a real module and use a pinned JSON library (none
is currently a dependency).

### A8 — Per-frame heap allocation in hot paths — **MEDIUM** (Rule "no per-frame alloc")
Confirmed `push_back`/per-callback allocation in frame/tick paths:
- `engine/render/pipeline/Extract.cpp:49` — `renderables.push_back` per frame
  (amortized by `clear()` retaining capacity, but unbounded on growth).
- `engine/script/behavior/BehaviorSpine.cpp:68` — `cmd.destroy.push_back` in
  `ProjectileBehavior::tick()`; a `reserve()` exists (`BehaviorSpine.h:46`) but `tick()`
  never calls it — relies on the caller.
- `engine/audio/.../MixerCore.h:635` — `std::vector<f32> raw(n_in)` **allocated every
  audio callback** (the clearest violation; audio callbacks are hard-real-time).
- Scene broadphase/cull outputs: `Bvh.cpp:220/231/242/253`, `UniformGrid.cpp:205/238/257`
  (`out.push_back`), plausibly per-frame.
- `engine/net/Lockstep.cpp:25`, `Reliability.cpp:107` — per-tick `push_back` (bounded by
  peers; low).
**Good counter-example:** `engine/net/Net.cpp:21-31` caches a `static PollScratch`.
**Fix:** pre-`reserve` at load and/or pool the audio scratch; the MixerCore one should
be fixed before any perf claim.

### A9 — Swap-remove makes intra-chunk order history-dependent — **LOW**
`erase_entity_at` swaps the last row into the freed slot
(`engine/scene/World.cpp:285-301`), so within-chunk entity order depends on
destroy history. Deterministic across peers only because identical inputs ⇒ identical
destroy order — but any system that depends on intra-chunk order, or any serializer
that captures chunk order, will diverge. Document "systems must not depend on
intra-chunk ordering."

### A10 — ADR-018 vs. reality drift — **LOW**
`ADR-018` (Accepted) describes a full Behavior IR → DOTS compiler with read/write-set
inference, loop fission, effect classification, a job scheduler running disjoint
write-sets across chunks, and dlopen hot-reload. What exists is: the `World` ECS, the
`MathLogicKernel` (`engine/math/MathLogicKernel.h`), a hand-lowered projectile spine
(`BehaviorSpine.*`), and `VisualGraphCompiler` which does emit Lua-preview + C++ text
(`engine/script/internal/VisualGraphCompiler.h:19-23`). There is **no scheduler, no
write-set inference, no parallel query, no loop-fission compiler.** That's fine for an
ADR — but the prose ("already emits", "the scheduler runs…") reads as further along
than the code. Mark the unbuilt parts as planned.

### A11 — "Determinism" test doesn't test determinism — **LOW** (coverage gap)
`tests/unit/scene_world_determinism.cpp` only covers handle-ABA (generation bump) and
world isolation — both good, but neither is cross-platform bit-determinism nor
component-ID stability, which `ADR-018:77` explicitly calls a hard requirement ("proven,
not assumed — golden replay tests across platforms"). No golden-replay test exists.

### What's genuinely good (brief)
- ECS holds archetypes via `unique_ptr` so `Archetype*` stays valid across archetype
  growth (`World.cpp:151,225`); the query path is stack-only and allocation-free
  (`kMaxQueryComponents=16`, `World.cpp:503-505`); chunks target 16 KB / 1024 entities
  (`World.cpp:18-19`); components `static_assert` trivial copyability.
- Hot paths are clean of exceptions, RTTI, `shared_ptr`, and inner-loop virtual dispatch
  (verified across render/scene/physics/audio/net/gpu/script/behavior). The one virtual
  interface (`render/post/upscale/IUpscaler.h`) is dispatched once per frame, not
  per-pixel.
- `MathLogicKernel` records once and executes over SoA scratch it owns — the no-alloc
  steady-state claim holds for the kernel itself.

---

## 3. Track B — Third-party / dependency findings

### Inventory

| Dependency | Version | Acquisition | License | Pinned? |
|---|---|---|---|---|
| fmt | 11.1.4 | `find_package` → FetchContent (`Dependencies.cmake:11-22`) | MIT | tag + `GIT_SHALLOW` (soft) |
| Catch2 | v3.7.1 | FetchContent (`Dependencies.cmake:43-46`) | BSL-1.0 | tag (soft) |
| zstd | host (homebrew 1.5.7) | `find_package(zstd CONFIG)` only, **no fallback** (`Dependencies.cmake:25-30`, `asset/CMakeLists.txt:9-22`) | BSD-3 / GPL-2 dual | **No — host-dependent** |
| Lua | 5.4 | `find_package` → FetchContent **URL + SHA256** (`script/CMakeLists.txt:22-25`) | MIT | ✅ **hash-pinned** |
| Vulkan headers/loader | host LunarG SDK | `find_package(Vulkan REQUIRED)` (`gpu/CMakeLists.txt:130`) | Apache-2.0/MIT | **No — host SDK** |
| volk | 1.4.304 | FetchContent (`gpu/CMakeLists.txt:142-145`) | MIT | tag + shallow (soft) |
| Jolt Physics | v5.5.0 | FetchContent, `CROSS_PLATFORM_DETERMINISTIC=ON` (`physics/core/CMakeLists.txt:114-117`) | MIT | tag + shallow (soft) — **determinism-critical** |
| Opus | v1.5.2 | FetchContent (`net/voice/CMakeLists.txt:10-13`) | BSD-3 | tag + shallow (soft) |
| FreeType | VER-2-13-3 | `find_package` → FetchContent (`ui/rml/CMakeLists.txt:41-44`) | FTL (not GPL) | tag (soft) |
| RmlUi | 6.2 | FetchContent (`ui/rml/CMakeLists.txt:57-60`) | MIT | tag (soft) |
| Slang (`slangc`) | unpinned | `find_program`, `custom_command` + runtime `popen` (`shader/CMakeLists.txt:23-55`) | MIT | **No — host binary** |
| metal-cpp | — | **NOT USED** — Metal backend is Obj-C++ on system SDK (`gpu/mtl/MetalBackend.mm:31-34`) | — | n/a (false NOTICE entry) |
| stb | — | vendored `third_party/stb/` | PD/MIT | vendored, no version |
| JetBrains Mono | — | vendored `third_party/fonts/` | OFL-1.1 | vendored |
| **spirv-cross** | — | only a fallback `find_program` binary probe (`shader/CMakeLists.txt:39`) | — | not a linked lib |
| **spirv-headers, glslang, shaderc, tinygltf, meshoptimizer, ktx, VMA** | — | **listed in `vcpkg.json`, wired NOWHERE** | — | dead manifest entries |

### B1 — `vcpkg.json` is not consumed; 7 entries are dead — **HIGH**
No `CMakePresets.json` preset sets `CMAKE_TOOLCHAIN_FILE`, so manifest-mode vcpkg never
runs; the documented build (`cmake --preset mac-debug`) resolves via
`find_package`(system/homebrew) + FetchContent. `spirv-headers`, `glslang`, `shaderc`,
`tinygltf`, `meshoptimizer`, `ktx`, and `vulkan-memory-allocator` appear in no
`CMakeLists`/`*.cmake`; `spirv-cross` only as an optional fallback binary.
**Fix:** pick one — either wire vcpkg for real (toolchain in presets) and prune the dead
entries, or delete `vcpkg.json` and document FetchContent as canonical. Today it
actively misleads (it looks like the dependency source of truth and isn't).

### B2 — `NOTICE` is substantially inaccurate — **HIGH** (legal/attribution)
`NOTICE` attributes vendored directories that **do not exist**: `third_party/jolt`,
`opus`, `freetype`, `rmlui`, `slang`, `vulkan-headers`, `volk`, `metal-cpp`
(`NOTICE:26-78`). All are FetchContent/`find_package`/system-SDK; metal-cpp is not used
at all. Conversely, actually-fetched deps are **missing**: fmt, Catch2 (BSL-1.0), Lua,
zstd, volk-as-fetched. The cross-referenced `docs/third-party-versions.md`
(`NOTICE:89`) **does not exist**. `third_party/README.md` is also stale (cites "Lane 15
script / 17 ui / 25 tests" vs. AGENTS.md's 20/21, and a non-existent GPL `quake-refs`).
**Why HIGH:** a dual-MIT/Apache project's `NOTICE` is its license-compliance artifact;
right now it can't be relied on for either completeness or correctness. **Fix:** rewrite
`NOTICE` to match the real (FetchContent/find_package) dependency set with correct
licenses, create `docs/third-party-versions.md` (or drop the reference), and refresh
`third_party/README.md`. *No copyleft leak was found* — FreeType uses FTL, the `*-refs`
trees are MIT sister code, and no GPL artifact is in the link graph.

### B3 — Determinism-relevant deps are not hash-pinned — **HIGH**
The lockstep pillar needs bit-identical physics across client/server/OS/arch. Jolt's
build flags are **exemplary** (`CROSS_PLATFORM_DETERMINISTIC=ON`, AVX-512 capped to AVX2
to avoid SIGILL, IPO forced off for cross-linker safety, `-fno-fast-math
-ffp-contract=off` on the consumer TUs — `physics/core/CMakeLists.txt:33,72-105,132-148,
172-177`). But Jolt is fetched by **mutable tag** + `GIT_SHALLOW`, and Vulkan + `slangc`
are whatever the host provides — so two dev boxes or CI runners can diverge. Only Lua is
hash-pinned. **Fix:** pin Jolt (and ideally every determinism-touching dep) by **commit
SHA**, and pin/verify the Vulkan SDK + Slang versions; add a lockfile or
`docs/third-party-versions.md` with exact commits per release.

### B4 — zstd is silent-optional and host-dependent (the macOS-26.0 warning) — **MEDIUM**
`Dependencies.cmake:29` claims "asset lane will FetchContent on demand" — **it does
not**; the asset lane only links zstd if a config package is already found, else
silently disables `.lmpak` compression (`asset/CMakeLists.txt:22`). On this host
`zstd_DIR=/opt/homebrew/lib/cmake/zstd` (homebrew 1.5.7), whose static lib was built for
the host's macOS 26.0 SDK but is linked at deployment target 14.0 — **the source of the
"built for newer macOS 26.0 than being linked (14.0)" linker warning.** Reproducibility
is host-dependent and compressed assets can silently vanish on a box without homebrew
zstd on the CMake prefix path. **Fix:** FetchContent a pinned zstd by URL+SHA256 (same
pattern as Lua), or vendor it; make it `REQUIRED` if `.lmpak` compression is a hard
feature.

### B5 — Slang is unpinned, host-resolved, and `popen`'d at runtime — **MEDIUM**
`find_program(SLANGC_EXECUTABLE …)` (`shader/CMakeLists.txt:23-30`) accepts whatever
`slangc` is on PATH/hints with **no version check** (the "2026.1" note is prose only),
bakes the path into `PSYNDER_SLANGC_PATH` (`:55`), and invokes it via `popen` at runtime.
Shader output — hence pipeline behavior — depends on the host's compiler version, and
runtime `popen` of an external binary is a deployment + (mild) security consideration.
**Fix:** pin a Slang release (FetchContent the binary by URL+hash), assert its version,
and prefer offline pre-compiled SPIR-V for shipping builds.

### B6 — Stale references / GPL surface — **LOW**
No GPL in the link graph (FreeType=FTL; `quake-refs` referenced by `third_party/README.md`
does not exist; `*-refs` are MIT). The README's lane numbers and the
`docs/third-party-versions.md` reference are stale — clean them up with B2.

### B7 — Supply-chain surface — **LOW**
~13 external deps, all over git/https; **only Lua is integrity-verified (SHA256)**.
`GIT_SHALLOW`+tag fetches (fmt, Catch2, volk, Jolt, opus, freetype, rmlui) can't detect a
moved tag and pull no verifiable hash. Parsing/network surface: opus (voice), freetype
(font parsing), zstd, plus the hand-rolled JSON parser (A7). Modest overall; the gap is
the absence of hashes.

### Vendoring strategy coherence — **incoherent**
`ADR-GX-016` describes shared foundation modules consumed as pinned deps, but the actual
build is a grab-bag: `find_package`(system) + FetchContent(tag) + one URL+hash (Lua) +
vendored stb/fonts + system Obj-C++ SDK + an unused vcpkg manifest. **Recommendation:**
commit to one strategy and write it into an ADR — either (a) vcpkg manifest + toolchain
in every preset (and prune dead entries), or (b) FetchContent with **commit-SHA** pins
and a per-release lockfile, deleting `vcpkg.json`. Given the determinism requirement, (b)
with SHAs + hashes is the safer default.

---

## 4. Prioritized action list

### Must fix before building the scene-on-ECS layer
1. **A1** — add the debug structural-change-during-iteration guard to `World`. (small)
2. **A3** — switch component IDs to a stable name-hash / explicit table. (small, prevents
   cross-binary rework of net + save + replay)
3. **Decide (design, no code):** authoritative scene representation; editor-document ↔
   ECS-component mapping; entity/component **wire/disk identity** (resolves A4). Without
   these the migration cannot be correct.

### Should fix before any networking / save / replay milestone
4. **B3** — pin Jolt (and Vulkan/Slang) by commit SHA; add `docs/third-party-versions.md`.
5. **A4** — define the snapshot entity identity (and stop truncating silently).
6. **A11** — add a golden cross-platform replay test (the determinism pillar is currently
   asserted, not tested).

### Should fix soon (correctness/repro/legal hygiene, cheap)
7. **B2** — rewrite `NOTICE` to the real dependency set; create the referenced versions
   doc; refresh `third_party/README.md`.
8. **B1** — resolve the vcpkg-vs-FetchContent split (wire it or delete it; prune 7 dead
   entries).
9. **B4** — pin zstd (URL+SHA256) and decide required-vs-optional; this also removes the
   macOS-version linker warning.
10. **A8** — pool/`reserve` the audio `MixerCore` per-callback scratch and the Extract /
    spatial-query output vectors.

### Can wait (debt / clarity)
11. **A2 (execution)** — the actual spine→ECS convergence (port the character controller,
    delete the bespoke primitive renderer) — large, do it *as* the migration.
12. **A7** — extract the editor/IPC/JSON out of the 4,701-line sample; use a real JSON lib.
13. **A5, A6, A9, A10, B5, B6, B7** — document invariants, fold duplicated CommandBuffers,
    align ADR-018 prose with reality, pin Slang.

---

*Evidence basis: build wiring (`CMakeLists.txt`, `CMakePresets.json`, `cmake/*.cmake`,
all lane `CMakeLists.txt`), `vcpkg.json`, `NOTICE`, the ECS core (`engine/scene/World.*`,
`CommandBuffer.*`, `engine/core/Types.h`), `MathLogicKernel.*`, `BehaviorSpine.*`,
`VisualGraphCompiler.*`, `engine/net/Snapshot.*`, `samples/02_crate/main.cpp`,
`samples/combat/Combat.h`, `docs/adr/ADR-018`, and targeted hot-path / dependency-trace
scans. No files were modified.*
