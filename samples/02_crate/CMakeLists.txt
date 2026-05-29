# SPDX-License-Identifier: MIT OR Apache-2.0
# sample_02_crate — one maintainable engine-loop sample.
#
# This is intentionally the only new sample for the next mini-wave. It is the
# integration target for ECS + camera + input + physics + render extraction,
# without growing a zoo of half-overlapping demos.

# main.cpp uses the editor's scene-authoring + IPC (LooseSceneDocument parsing,
# runtime console / web IPC), so this sample requires the editor. It is built
# under PSYNDER_GX_BUILD_SAMPLES regardless of the editor flag, so skip it when
# the editor is off (e.g. CI's engine+tests matrix) rather than link-erroring.
if(NOT TARGET psynder_editor)
    message(STATUS "sample_02_crate: skipped (requires PSYNDER_GX_EDITOR=ON for scene authoring + IPC)")
    return()
endif()

add_executable(sample_02_crate main.cpp)
target_compile_definitions(sample_02_crate PRIVATE
    "PSYNDER_GX_UI_IMM_SHADER_PATH=\"${CMAKE_SOURCE_DIR}/engine/ui/imm/shaders/imm_overlay.slang\""
    "PSYNDER_GX_PRIMITIVE_SHADER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/primitive_unlit.slang\"")

target_link_libraries(sample_02_crate PRIVATE
    psynder_common
    psynder_core
    psynder_math
    psynder_jobs
    psynder_asset
    psynder_camera
    psynder_scene
    psynder_physics_core
    psynder_physics_destruction
    psynder_physics_agents
    psynder_script
)

if(TARGET psynder_platform)
    target_link_libraries(sample_02_crate PRIVATE psynder_platform)
endif()
if(TARGET psynder_gpu)
    target_link_libraries(sample_02_crate PRIVATE psynder_gpu)
endif()
if(TARGET psynder_shader)
    target_link_libraries(sample_02_crate PRIVATE psynder_shader)
endif()
if(TARGET psynder_ui_imm)
    target_link_libraries(sample_02_crate PRIVATE psynder_ui_imm)
endif()
if(TARGET psynder_editor)
    target_link_libraries(sample_02_crate PRIVATE psynder_editor)
endif()
