# SPDX-License-Identifier: MIT OR Apache-2.0
# Psynder-GX — GPU-accelerated competitive FPS engine.
# Root CMakeLists. The 25 lane subdirectories are added in dependency order
# per DESIGN-PSYNDER-GX.md §19.5. Each subdir owns its own CMakeLists.txt;
# this file is owned by the orchestrator and orchestrates dependencies.

cmake_minimum_required(VERSION 3.28)

# ─── Project ──────────────────────────────────────────────────────────────
project(psynder_gx
    VERSION     0.1.0
    DESCRIPTION "GPU-accelerated competitive FPS engine — Vulkan + native Metal, C++23"
    LANGUAGES   C CXX
)

# Enable Objective-C++ on macOS for Metal backend (.mm files)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJCXX)
endif()

# ─── Project-level options ────────────────────────────────────────────────
option(PSYNDER_GX_EDITOR              "Build editor / sandbox mode + IPC server + React panels"  ON)
option(PSYNDER_GX_DEDICATED_SERVER    "Build headless dedicated server (strips render/audio/editor)" OFF)
option(PSYNDER_GX_BUILD_TOOLS         "Build offline tools (lm_pak, lm_cook_gx, lm_qbsp, lm_bake_gx)" ON)
option(PSYNDER_GX_BUILD_PLAYER        "Build the PsyArcadeGX player executable"                      ON)
option(PSYNDER_GX_BUILD_SAMPLES       "Build sample programs under samples/"                     ON)
option(PSYNDER_GX_BUILD_TESTS         "Build unit / golden / netcode / bench tests"              ON)
option(PSYNDER_GX_ENABLE_TRACY        "Enable Tracy profiler integration (debug builds default)" OFF)
option(PSYNDER_GX_ENABLE_SANITIZERS   "Enable ASan + UBSan (debug Linux/Mac only)"               OFF)
option(PSYNDER_GX_ENABLE_AVX2         "Build x86-64 AVX2 fast-path kernels"                       ON)
option(PSYNDER_GX_ENABLE_AVX512       "Build x86-64 AVX-512 wide-path kernels (opportunistic)"    ON)
# AVX-512 is ON by default because lane 03-simd uses per-function target
# attributes (__attribute__((target("avx512f,avx512bw,avx512vl")))) on every
# wide kernel in Simd_avx512.cpp.  The compiler only emits AVX-512 inside
# those decorated functions; auto-vectorisation elsewhere stays at the SSE4.2
# / AVX2 baseline, and Dispatch.cpp gates every call on the runtime tier
# (Tier::Avx512, from CpuFeatures::avx512f) — so a binary built with this ON
# still runs correctly on pre-Ice-Lake / non-AVX-512 hosts.
#
# SCOPE: this flag governs ONLY the simd lane's runtime-dispatched kernels.
# It must NOT be forwarded to third-party libraries that have no runtime ISA
# dispatch (they would emit AVX-512 unconditionally and SIGILL on CPUs without
# it). Jolt is the cautionary case — see engine/physics/core/CMakeLists.txt,
# which deliberately pins Jolt's USE_AVX512 OFF and caps it at AVX2.
option(PSYNDER_GX_ENABLE_LTO          "Link-time optimization for release builds"                 ON)
option(PSYNDER_GX_ENABLE_RT           "Enable hardware ray-tracing pipeline (RTX 20+, RDNA2+, M3+)" ON)
option(PSYNDER_GX_ENABLE_MESH_SHADERS "Enable mesh-shader pipeline (Turing+, RDNA2+, M3+)"        ON)

# Dedicated-server build strips graphics, audio, editor
if(PSYNDER_GX_DEDICATED_SERVER)
    set(PSYNDER_GX_EDITOR        OFF CACHE BOOL "" FORCE)
    set(PSYNDER_GX_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
endif()

# Back-compat aliases so vendored Psynder code (which uses PSYNDER_*) still works.
# Lane agents should write new code against PSYNDER_GX_*.
set(PSYNDER_EDITOR            ${PSYNDER_GX_EDITOR})
set(PSYNDER_BUILD_TOOLS       ${PSYNDER_GX_BUILD_TOOLS})
set(PSYNDER_BUILD_PLAYER      ${PSYNDER_GX_BUILD_PLAYER})
set(PSYNDER_BUILD_SAMPLES     ${PSYNDER_GX_BUILD_SAMPLES})
set(PSYNDER_BUILD_TESTS       ${PSYNDER_GX_BUILD_TESTS})
set(PSYNDER_ENABLE_TRACY      ${PSYNDER_GX_ENABLE_TRACY})
set(PSYNDER_ENABLE_SANITIZERS ${PSYNDER_GX_ENABLE_SANITIZERS})
set(PSYNDER_ENABLE_AVX2       ${PSYNDER_GX_ENABLE_AVX2})
set(PSYNDER_ENABLE_AVX512     ${PSYNDER_GX_ENABLE_AVX512})
set(PSYNDER_ENABLE_LTO        ${PSYNDER_GX_ENABLE_LTO})

# ─── C++23, no exceptions / no RTTI in hot subsystems (per §3 DOTS rules) ─
set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)

# Always-on per-target compile features
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS   ON)

# Place all binaries under build/${preset}/bin so samples + tests find them
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ─── Module path ──────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ─── Compiler warnings & sanitizers ───────────────────────────────────────
include(CompilerWarnings)
include(Sanitizers)
include(HotLane)
include(Dependencies)
include(Lanes)
include(Goldens)

# ─── Platform detection helpers ───────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PSYNDER_GX_PLATFORM_MACOS TRUE)
    set(PSYNDER_PLATFORM_MACOS    TRUE)  # back-compat for vendored Psynder code
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PSYNDER_GX_PLATFORM_WIN32 TRUE)
    set(PSYNDER_PLATFORM_WIN32    TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PSYNDER_GX_PLATFORM_LINUX TRUE)
    set(PSYNDER_PLATFORM_LINUX    TRUE)
else()
    message(FATAL_ERROR "Psynder-GX targets Win/Linux/macOS only; unsupported: ${CMAKE_SYSTEM_NAME}")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(PSYNDER_GX_ARCH_ARM64 TRUE)
    set(PSYNDER_ARCH_ARM64    TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(PSYNDER_GX_ARCH_X64 TRUE)
    set(PSYNDER_ARCH_X64    TRUE)
else()
    message(WARNING "Untested CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

message(STATUS "Psynder-GX target: ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Psynder-GX editor mode:      ${PSYNDER_GX_EDITOR}")
message(STATUS "Psynder-GX dedicated server: ${PSYNDER_GX_DEDICATED_SERVER}")
message(STATUS "Psynder-GX hardware RT:      ${PSYNDER_GX_ENABLE_RT}")
message(STATUS "Psynder-GX mesh shaders:     ${PSYNDER_GX_ENABLE_MESH_SHADERS}")

# ─── Project identity flag (CMake-side) ───────────────────────────────────
# Some lane CMakeLists vendored from sister Psynder branch on `if(PSYNDER_GX)`
# to gate the GPU-engine path (Vulkan / Metal) vs Psynder's CPU path.
# PSYNDER_GX=1 the compile-time define above is the runtime gate; this
# variable is the CMake-time gate that those lane scripts expect.
set(PSYNDER_GX TRUE)

# ─── psynder_common interface target — propagates settings to every lib ──
add_library(psynder_common INTERFACE)
target_compile_features(psynder_common INTERFACE cxx_std_23)
psynder_set_warnings(psynder_common)
target_include_directories(psynder_common INTERFACE
    ${CMAKE_SOURCE_DIR}/engine
)
target_compile_definitions(psynder_common INTERFACE
    PSYNDER_GX=1
)
if(PSYNDER_GX_EDITOR)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_EDITOR=1 PSYNDER_GX_EDITOR=1)
endif()
if(PSYNDER_GX_DEDICATED_SERVER)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_GX_DEDICATED_SERVER=1)
endif()
if(PSYNDER_GX_ENABLE_RT)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_GX_ENABLE_RT=1)
endif()
if(PSYNDER_GX_ENABLE_MESH_SHADERS)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_GX_ENABLE_MESH_SHADERS=1)
endif()
if(PSYNDER_GX_PLATFORM_MACOS)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_PLATFORM_MACOS=1 PSYNDER_GX_PLATFORM_MACOS=1)
endif()
if(PSYNDER_GX_PLATFORM_WIN32)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_PLATFORM_WIN32=1 PSYNDER_GX_PLATFORM_WIN32=1)
endif()
if(PSYNDER_GX_PLATFORM_LINUX)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_PLATFORM_LINUX=1 PSYNDER_GX_PLATFORM_LINUX=1)
endif()
if(PSYNDER_GX_ARCH_X64)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_ARCH_X64=1 PSYNDER_GX_ARCH_X64=1)
endif()
if(PSYNDER_GX_ARCH_ARM64)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_ARCH_ARM64=1 PSYNDER_GX_ARCH_ARM64=1)
endif()

# ─── Engine subdirs — strict dependency order per DESIGN §19.5 ───────────
#   core → math → simd → jobs → asset → scene → gpu → shader →
#   render-pipeline → render-rt → render-post → audio → script → physics →
#   net → net-voice → editor → ui → world → platform → game
#
# Subdirs are added bottom-up so dependents see their dep targets already
# in scope. Lane subdirs may add their own subdirectory CMakeLists.

add_subdirectory(engine/core)
add_subdirectory(engine/math)
add_subdirectory(engine/simd)
add_subdirectory(engine/jobs)
add_subdirectory(engine/asset)
add_subdirectory(engine/scene)
# camera depends only on core+math; safe to add this early so dependent
# lanes (samples, render) can `target_link_libraries(... psynder_camera)`.
add_subdirectory(engine/camera)

# GPU + shader come before the renderer subdirs that depend on them
if(NOT PSYNDER_GX_DEDICATED_SERVER)
    add_subdirectory(engine/gpu)
    add_subdirectory(engine/shader)
    add_subdirectory(engine/render)
endif()

add_subdirectory(engine/world)
add_subdirectory(engine/worldstate)
add_subdirectory(engine/physics)

if(NOT PSYNDER_GX_DEDICATED_SERVER)
    add_subdirectory(engine/audio)
endif()

add_subdirectory(engine/net)
add_subdirectory(engine/script)

if(NOT PSYNDER_GX_DEDICATED_SERVER)
    add_subdirectory(engine/ui)
endif()

add_subdirectory(engine/platform)

if(PSYNDER_GX_EDITOR)
    add_subdirectory(engine/editor)
endif()

# ─── Tools / samples / tests ──────────────────────────────────────────────
if(PSYNDER_GX_BUILD_PLAYER AND NOT PSYNDER_GX_DEDICATED_SERVER)
    add_subdirectory(apps)
endif()
if(PSYNDER_GX_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(PSYNDER_GX_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()
if(PSYNDER_GX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Summary ──────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "── Psynder-GX build summary ───────────────────────────")
message(STATUS "  Editor mode:        ${PSYNDER_GX_EDITOR}")
message(STATUS "  Dedicated server:   ${PSYNDER_GX_DEDICATED_SERVER}")
message(STATUS "  Hardware RT:        ${PSYNDER_GX_ENABLE_RT}")
message(STATUS "  Mesh shaders:       ${PSYNDER_GX_ENABLE_MESH_SHADERS}")
message(STATUS "  Tools:              ${PSYNDER_GX_BUILD_TOOLS}")
message(STATUS "  Player:             ${PSYNDER_GX_BUILD_PLAYER}")
message(STATUS "  Samples:            ${PSYNDER_GX_BUILD_SAMPLES}")
message(STATUS "  Tests:              ${PSYNDER_GX_BUILD_TESTS}")
message(STATUS "  Tracy:              ${PSYNDER_GX_ENABLE_TRACY}")
message(STATUS "  Sanitizers:         ${PSYNDER_GX_ENABLE_SANITIZERS}")
message(STATUS "  LTO:                ${PSYNDER_GX_ENABLE_LTO}")
message(STATUS "──────────────────────────────────────────────────────")
message(STATUS "")
