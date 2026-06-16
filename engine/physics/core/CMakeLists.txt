# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_physics_core — Lane 15.
# Jolt Physics wrapper, fixed 120 Hz tick, character controller (capsule,
# crouch/prone/lean/ladder/water). Deterministic across platforms
# (-fno-fast-math + pinned FP rounding + Jolt CROSS_PLATFORM_DETERMINISTIC).
# See DESIGN-PSYNDER-GX.md §10.1.

# ─── Vendor Jolt Physics (MIT) via FetchContent ──────────────────────────
# Pin an exact commit SHA (see FetchContent_Declare below), not a mutable
# tag or main, so builds stay reproducible — determinism is the engine's #1
# pillar and now rides on Jolt (ADR-019). Sources land under third_party/jolt/
# so multiple build directories share one checkout and the tree mirrors the
# layout PLAN §10.1 documents.
#
# Jolt's top-level CMake lives in its Build/ subdir; consumers point
# FetchContent at SOURCE_SUBDIR Build to skip the rest of the upstream
# tree (samples / hello-world / etc.).
include(FetchContent)

option(PSYNDER_GX_VENDOR_JOLT
    "Vendor Jolt Physics via FetchContent under third_party/jolt/."
    ON
)

if(PSYNDER_GX_VENDOR_JOLT AND NOT TARGET Jolt)
    set(FETCHCONTENT_QUIET FALSE)
    message(STATUS "[physics-core] Jolt Physics v5.5.0 via FetchContent")

    # Jolt-side option toggles — set BEFORE FetchContent_MakeAvailable so
    # they take effect during the subproject configure.
    #
    # CROSS_PLATFORM_DETERMINISTIC=ON is the load-bearing flag for lockstep
    # replay / netcode reproducibility (DESIGN §10.1, §14). It selects the
    # deterministic SIMD code paths and disables fast-math inside Jolt.
    set(CROSS_PLATFORM_DETERMINISTIC ON  CACHE BOOL "" FORCE)
    # Match our project-wide DOTS contract: no exceptions, no RTTI in hot
    # subsystems (physics qualifies).
    set(CPP_EXCEPTIONS_ENABLED       OFF CACHE BOOL "" FORCE)
    set(CPP_RTTI_ENABLED             OFF CACHE BOOL "" FORCE)
    # Match the engine's CRT. The rest of Psynder-GX builds against the
    # dynamic MSVC runtime (/MD — CMake's default with no explicit
    # CMAKE_MSVC_RUNTIME_LIBRARY). Jolt defaults USE_STATIC_MSVC_RUNTIME_LIBRARY
    # to ON (/MT), which makes Jolt.lib /MT while every other lib is /MD; any
    # target linking both (psynder_unit) then fails with
    # `lld-link: error: /failifmismatch: mismatch detected for 'RuntimeLibrary'`.
    # Force OFF so Jolt uses /MD too. MSVC-only; ignored on Linux/macOS.
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    # Disable upstream samples, viewer, perf tests, hello-world and unit
    # tests — we don't need them and they pull in extra deps (e.g. GLFW).
    set(TARGET_UNIT_TESTS            OFF CACHE BOOL "" FORCE)
    set(TARGET_HELLO_WORLD           OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST      OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES               OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER                OFF CACHE BOOL "" FORCE)
    # Keep Jolt's allocator hooks and the embedded ObjectStream so tools
    # can serialize physics state later (M4+).
    set(DISABLE_CUSTOM_ALLOCATOR     OFF CACHE BOOL "" FORCE)
    set(ENABLE_OBJECT_STREAM         ON  CACHE BOOL "" FORCE)
    set(GENERATE_DEBUG_SYMBOLS       ON  CACHE BOOL "" FORCE)
    # OBJECT_LAYER_BITS=16 is fine for our ~10 layer count; expand to 32
    # later if needed.
    set(OBJECT_LAYER_BITS            16  CACHE STRING "" FORCE)
    # Leave OVERRIDE_CXX_FLAGS OFF so Jolt does not stomp on our top-level
    # compile flags. We supply -fno-fast-math + -ffp-contract=off on the
    # consumer side via target_compile_options below, and Jolt's own TUs
    # honour CROSS_PLATFORM_DETERMINISTIC for its internal code.
    set(OVERRIDE_CXX_FLAGS           OFF CACHE BOOL "" FORCE)
    # Don't install Jolt — we link the static library locally.
    set(ENABLE_INSTALL               OFF CACHE BOOL "" FORCE)

    # Selectively enable x86 ISA extensions only on x86_64. arm64 (Apple
    # Silicon, Linux ARM) ignores these but Jolt's logic still gates on
    # them at configure time.
    if(PSYNDER_GX_ARCH_X64)
        set(USE_SSE4_1 ON  CACHE BOOL "" FORCE)
        set(USE_SSE4_2 ON  CACHE BOOL "" FORCE)
        set(USE_AVX    ON  CACHE BOOL "" FORCE)
        set(USE_AVX2   ${PSYNDER_GX_ENABLE_AVX2}   CACHE BOOL "" FORCE)
        # Jolt has NO runtime ISA dispatch: USE_AVX512 compiles libJolt.a
        # TU-wide with -mavx512* and assumes the host CPU supports it. Since
        # libJolt.a is linked into psynder_unit, forwarding
        # PSYNDER_GX_ENABLE_AVX512 here SIGILL'd the test binary at startup on
        # non-AVX-512 CPUs (e.g. the GitHub CI runners) — a Jolt global
        # initialiser executed an AVX-512 instruction before main(). Cap Jolt
        # at AVX2 (the project's guaranteed runtime baseline) regardless of the
        # AVX-512 option. The opportunistic AVX-512 path lives ONLY in the simd
        # lane (engine/simd/Simd_avx512.cpp), which IS runtime-dispatched
        # (Tier::Avx512 gated on CpuFeatures::avx512f) and is therefore safe to
        # keep enabled by default.
        set(USE_AVX512 OFF CACHE BOOL "" FORCE)
        set(USE_LZCNT  ON  CACHE BOOL "" FORCE)
        set(USE_TZCNT  ON  CACHE BOOL "" FORCE)
        set(USE_F16C   ON  CACHE BOOL "" FORCE)
        set(USE_FMADD  ON  CACHE BOOL "" FORCE)
    else()
        # Avoid Jolt trying to enable x86 ISA on arm64 — leaving these
        # OFF picks the NEON code path on Apple Silicon automatically.
        set(USE_SSE4_1 OFF CACHE BOOL "" FORCE)
        set(USE_SSE4_2 OFF CACHE BOOL "" FORCE)
        set(USE_AVX    OFF CACHE BOOL "" FORCE)
        set(USE_AVX2   OFF CACHE BOOL "" FORCE)
        set(USE_AVX512 OFF CACHE BOOL "" FORCE)
        set(USE_LZCNT  OFF CACHE BOOL "" FORCE)
        set(USE_TZCNT  OFF CACHE BOOL "" FORCE)
        set(USE_F16C   OFF CACHE BOOL "" FORCE)
        set(USE_FMADD  OFF CACHE BOOL "" FORCE)
    endif()

    # NOTE: no in-tree SOURCE_DIR — the root .gitignore doesn't list
    # third_party/jolt/ (orchestrator-owned), and the upstream checkout
    # is ~60 MB. Letting FetchContent stash it under each build dir's
    # _deps/ keeps the repo clean. Per build dir adds ~60 MB; acceptable
    # for the bootstrap. If the orchestrator later adds an ignore rule,
    # we can switch back to SOURCE_DIR third_party/jolt to dedupe across
    # build dirs.
    # Determinism is the engine's #1 pillar and now rides on Jolt (ADR-019).
    # Pin the EXACT upstream commit, not the mutable v5.5.0 tag — a tag can be
    # force-moved or re-cut upstream, which would silently change physics
    # behaviour and break bitwise lockstep replay. 23dadd0e is the commit that
    # the v5.5.0 tag pointed at when this pin was made (verified via
    # `git ls-remote --tags https://github.com/jrouwe/JoltPhysics.git v5.5.0`).
    # Same source, same behaviour — just reproducible. Bump this SHA (and the
    # comment) deliberately when upgrading Jolt, re-running the golden/
    # determinism suite afterwards (ARCH-review B3, GH #39).
    #
    # GIT_SHALLOW is OFF here on purpose: a shallow fetch keyed on a raw SHA
    # relies on the server honouring `fetch <sha>`, which is not universally
    # supported and can fail mid-CI. A full fetch always resolves the SHA.
    FetchContent_Declare(JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG        23dadd0e603f1b321142d4c74df07fce85064989 # v5.5.0
        GIT_SHALLOW    FALSE
        SOURCE_SUBDIR  Build
    )
    # Jolt's Build/CMakeLists.txt calls check_ipo_supported() at configure
    # time and on success sets CMAKE_INTERPROCEDURAL_OPTIMIZATION_<CONFIG>
    # globally for Release / RelWithDebInfo / MinSizeRel. That causes Jolt
    # to compile with -flto and libJolt.a then contains LLVM bitcode.
    # Apple Clang + lld on macOS handle that fine; GNU /usr/bin/ld on
    # Linux 24.04 doesn't, and psynder_unit's link explodes with:
    #   /usr/bin/ld: lib/libJolt.a: error adding symbols: file format not
    #   recognized
    # Force the cache vars OFF BEFORE FetchContent_MakeAvailable so Jolt's
    # configure can't turn IPO back on. Setting only the target property
    # post-fetch is too late — Jolt has already baked -flto into its
    # compile flags by then.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        OFF CACHE BOOL "" FORCE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF CACHE BOOL "" FORCE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(JoltPhysics)

    # Belt-and-suspenders: also flip the target property OFF in case Jolt
    # added it explicitly to its own target rather than relying on the
    # CMAKE_* global.
    if(TARGET Jolt)
        set_target_properties(Jolt PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION                FALSE
            INTERPROCEDURAL_OPTIMIZATION_RELEASE        FALSE
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO FALSE
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     FALSE
        )
    endif()
endif()

# ─── Lane static library ─────────────────────────────────────────────────
# psynder_add_lane globs every .cpp under engine/physics/core/ into a
# static library named psynder_physics_core. The wrapper TU that lives at
# JoltCore.cpp implements every symbol in PublicPhysicsCore.h on top of
# Jolt's PhysicsSystem / BodyInterface / CharacterVirtual.
psynder_add_lane(physics_core
    LINKS
        psynder_core psynder_math psynder_simd psynder_jobs psynder_scene
)

# Link Jolt into the lane library. PUBLIC because our wrapper's compile
# needs Jolt headers reachable via include paths even though no Jolt
# types leak through PublicPhysicsCore.h to downstream lanes.
if(TARGET Jolt)
    target_link_libraries(psynder_physics_core PUBLIC Jolt)
    target_compile_definitions(psynder_physics_core PRIVATE PSYNDER_GX_HAS_JOLT=1)
endif()

# ─── Determinism: physics TUs compiled with strict FP (DESIGN §14) ──────
# Combined with Jolt's CROSS_PLATFORM_DETERMINISTIC, this gives bitwise
# reproducible ticks across arm64 / x86_64 / MSVC — required for lockstep
# replay. Centralized in cmake/HotLane.cmake so the MSVC `/fp:strict` branch
# is never missed (it previously was, here, while the net lane had it).
psynder_determinism_fp(psynder_physics_core)

# ─── Hook this lane onto the shared unit-test binary ────────────────────
# tests/unit/CMakeLists.txt is owned by lane 25 (tests) and built only
# when PSYNDER_GX_BUILD_TESTS=ON. Defer the link wiring to end-of-
# configure so we attach without editing files outside our lane.
# Mirrors the pattern used by lane 17 (ui-rml) and the script lane.
function(psynder_physics_core_attach_to_unit_target)
    if(TARGET psynder_unit)
        target_link_libraries(psynder_unit PRIVATE psynder_physics_core)
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
    psynder_physics_core_attach_to_unit_target)
