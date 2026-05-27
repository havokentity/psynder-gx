# SPDX-License-Identifier: MIT OR Apache-2.0
# sample_00_clear — M0 deliverable.
# Opens a window, presents an animated clear color via psy::gpu::Device.
# Acceptance: runs on Win/Linux Vulkan + macOS native Metal from one binary.

add_executable(sample_00_clear main.cpp)
target_compile_definitions(sample_00_clear PRIVATE
    "PSYNDER_GX_UI_IMM_SHADER_PATH=\"${CMAKE_SOURCE_DIR}/engine/ui/imm/shaders/imm_overlay.slang\"")
target_link_libraries(sample_00_clear PRIVATE
    psynder_common
    psynder_core
    psynder_math
)

# On macOS, the actual Metal-layer + window creation happens in lane 25
# (engine/platform/macos/). On Win/Linux, lane 23/24 hook the Vulkan
# surface. The platform aggregator (engine/platform/CMakeLists.txt) wires
# the right one in.
if(TARGET psynder_platform)
    target_link_libraries(sample_00_clear PRIVATE psynder_platform)
endif()
if(TARGET psynder_gpu)
    target_link_libraries(sample_00_clear PRIVATE psynder_gpu)
endif()
# psynder_gpu's Vulkan init publishes its device to lane 08 via
# psynder_gx_shader_push_vk_context (defined in psynder_shader), so every GPU
# consumer must resolve that symbol even with no shaders of its own.
if(TARGET psynder_shader)
    target_link_libraries(sample_00_clear PRIVATE psynder_shader)
endif()
if(TARGET psynder_ui_imm)
    target_link_libraries(sample_00_clear PRIVATE psynder_ui_imm)
endif()
