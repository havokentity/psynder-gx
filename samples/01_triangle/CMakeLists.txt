# SPDX-License-Identifier: MIT OR Apache-2.0
# sample_01_triangle — M1 deliverable.
#
# Renders a 1 m tall textured triangle that rotates around the world-space
# Y axis at ~69 deg/s. Exercises:
#   - lane 25 / 23 window + native handle ABI (NSWindow + CAMetalLayer on
#     macOS; HWND on Win32; Linux placeholder).
#   - lane 07 GPU device + swapchain + cmd encoder API (begin_render,
#     bind_pipeline, bind_vertex_buffer, bind_index_buffer, push_constants,
#     draw_indexed, end_render).
#   - lane 07 buffer / texture / sampler creation via the handle ABI.
#   - lane 08 slang compile (triangle_textured.slang → SPIR-V on the host).
#   - lane 02 math (perspective_rh, look_at_rh, rotate_quat, mul).
#   - lane 04 jobs system bring-up (per the DOTS+jobs mandate; the M1 demo
#     itself fans no work out but the loop must route through the
#     psynder::jobs::JobSystem singleton).
#
# Acceptance: runs on macOS / Win32 / Linux from a single source file. On
# Linux the visual is gated on the lane 24 ⇄ lane 07 tagged-handle ABI;
# the CI smoke-frames path exits 0 so the build stays green.

add_executable(sample_01_triangle main.cpp)

# Enable the real M1 draw path (begin_render -> bind_pipeline -> draw_indexed)
# on Vulkan targets. The sample defaults to the M0 clear-only fallback to dodge
# a macOS-only Metal encoder-retain crash (sample01-004) -- that bug does not
# exist on the Vulkan backend, and the PSO registration the draw needs is
# provided by lane 08's pipeline_shim (lane/08-pso-register). Keep the fallback
# on Apple until the Metal retain fix (lane/07-mtl-encoder-retain) lands.
if(NOT APPLE)
    target_compile_definitions(sample_01_triangle PRIVATE PSYNDER_GX_SAMPLE_01_USE_ENCODER)
endif()

target_link_libraries(sample_01_triangle PRIVATE
    psynder_common
    psynder_core
    psynder_math
    psynder_jobs
)

# Platform + GPU libraries pulled in via the orchestrator-owned aggregators
# (engine/platform/CMakeLists.txt + engine/gpu/CMakeLists.txt). We mirror
# the conditional guards from sample_00_clear so the link line stays small
# on host configurations that haven't enabled the GPU stack (dedicated
# server builds set PSYNDER_GX_DEDICATED_SERVER and strip these targets).
if(TARGET psynder_platform)
    target_link_libraries(sample_01_triangle PRIVATE psynder_platform)
endif()
if(TARGET psynder_gpu)
    target_link_libraries(sample_01_triangle PRIVATE psynder_gpu)
endif()
if(TARGET psynder_shader)
    target_link_libraries(sample_01_triangle PRIVATE psynder_shader)
endif()

# Make the shader source visible in IDE project trees so the slang file is
# editable from the sample folder. The shader is compiled at runtime by
# lane 08's slangc subprocess driver — there is no build-time SPIR-V step
# inside this sample, mirroring how the M0 sample relies on the engine's
# shader cache at first-use.
target_sources(sample_01_triangle PRIVATE
    shaders/triangle_textured.slang
)
set_source_files_properties(shaders/triangle_textured.slang PROPERTIES
    HEADER_FILE_ONLY ON
)
