# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_shader — Lane 08.
#
# Slang shader compiler integration. Emits SPIR-V (for Vulkan) + Metal IR
# (for Apple Silicon). Pipeline state objects (VkPipeline / MTLRenderPipelineState)
# precompiled at build, cached on first run. Filesystem watcher drives hot
# reload between frames.
#
# Toolchain decision (DESIGN §2.2):
#   PRIMARY:  slangc binary (2026.1) installed at /usr/local/bin/slangc.
#             We locate it via find_program and invoke it as a subprocess
#             (both at build time for pre-baking builtins, and at runtime
#             via popen). We do NOT use find_package(slang CONFIG) because
#             the installed slangTargets.cmake has a broken doubled-lib path
#             (/usr/local/lib/lib/...) in its MODULE targets; using the
#             binary avoids that CMake config bug entirely.
#             slang.h is still available at /usr/local/include/slang.h for
#             optional C API use; we expose it as an INTERFACE include dir.
#   FALLBACK: glslc + spirv-cross (both in vcpkg.json / /usr/local).
#             Activated if slangc binary not found.

# ── Locate slangc binary ──────────────────────────────────────────────────
find_program(SLANGC_EXECUTABLE
    NAMES slangc
    HINTS
        /usr/local/bin
        "$ENV{VULKAN_SDK}/bin"
        "${CMAKE_PREFIX_PATH}/bin"
    DOC "Path to the slangc shader compiler"
)

if(SLANGC_EXECUTABLE)
    message(STATUS "[shader] slangc found: ${SLANGC_EXECUTABLE} — Slang primary path active")
    set(PSYNDER_SHADER_USE_SLANG TRUE)
    set(PSYNDER_SLANG_INC_DIR /usr/local/include)
else()
    # Fallback: glslc + spirv-cross
    find_program(GLSLC_EXECUTABLE NAMES glslc DOC "glslc GLSL/HLSL → SPIR-V compiler")
    find_program(SPIRVCROSS_EXECUTABLE NAMES spirv-cross DOC "spirv-cross SPIR-V reflector/transpiler")
    if(GLSLC_EXECUTABLE)
        message(STATUS "[shader] slangc absent; glslc fallback: ${GLSLC_EXECUTABLE}")
    else()
        message(WARNING "[shader] Neither slangc nor glslc found — shader compilation will be stub-only")
    endif()
    set(PSYNDER_SHADER_USE_SLANG FALSE)
    set(PSYNDER_SLANG_INC_DIR "")
endif()

# ── Build the lane library ─────────────────────────────────────────────────
psynder_add_lane(shader
    LINKS
        psynder_core psynder_jobs psynder_asset psynder_gpu
    PRIVATE_DEFS
        $<$<BOOL:${PSYNDER_SHADER_USE_SLANG}>:PSYNDER_SHADER_HAVE_SLANG=1>
        $<$<BOOL:${SLANGC_EXECUTABLE}>:PSYNDER_SLANGC_PATH="${SLANGC_EXECUTABLE}">
)

# Expose slang.h to private compilation units in this lane.
# Other lanes never include slang.h directly — they use PublicShader.h.
if(PSYNDER_SLANG_INC_DIR)
    target_include_directories(psynder_shader PRIVATE "${PSYNDER_SLANG_INC_DIR}")
endif()

# ── Install the builtin shaders alongside the engine ──────────────────────
# Shader source files are included in-tree; no install step needed for M0–M1.
# They are referenced at runtime via path relative to the repo root, or via
# the VFS root in later milestones.

# ── Compile passthrough.slang at build time to verify toolchain (M1) ──────
if(SLANGC_EXECUTABLE)
    set(_passthrough_slang
        "${CMAKE_CURRENT_SOURCE_DIR}/builtin/passthrough.slang")
    set(_passthrough_vs_spv
        "${CMAKE_CURRENT_BINARY_DIR}/builtin/passthrough_vs.spv")
    set(_passthrough_fs_spv
        "${CMAKE_CURRENT_BINARY_DIR}/builtin/passthrough_fs.spv")

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/builtin")

    # Vertex stage
    add_custom_command(
        OUTPUT  "${_passthrough_vs_spv}"
        COMMAND "${SLANGC_EXECUTABLE}"
                "${_passthrough_slang}"
                -entry vs_main
                -stage vertex
                -profile glsl_450
                -target spirv
                -o "${_passthrough_vs_spv}"
        DEPENDS "${_passthrough_slang}"
        COMMENT "[shader] passthrough.slang vs_main → SPIR-V"
        VERBATIM
    )

    # Fragment stage
    add_custom_command(
        OUTPUT  "${_passthrough_fs_spv}"
        COMMAND "${SLANGC_EXECUTABLE}"
                "${_passthrough_slang}"
                -entry fs_main
                -stage fragment
                -profile glsl_450
                -target spirv
                -o "${_passthrough_fs_spv}"
        DEPENDS "${_passthrough_slang}"
        COMMENT "[shader] passthrough.slang fs_main → SPIR-V"
        VERBATIM
    )

    # Custom target that bakes the built-in shaders
    add_custom_target(psynder_shader_builtins
        DEPENDS
            "${_passthrough_vs_spv}"
            "${_passthrough_fs_spv}"
        COMMENT "[shader] Built-in shaders baked to SPIR-V"
    )

    # Wire builtins into the main psynder_shader build so
    # cmake --build ... --target psynder_shader bakes them automatically.
    add_dependencies(psynder_shader psynder_shader_builtins)

    message(STATUS "[shader] Built-in passthrough will bake to:")
    message(STATUS "  VS spv: ${_passthrough_vs_spv}")
    message(STATUS "  FS spv: ${_passthrough_fs_spv}")
endif()
