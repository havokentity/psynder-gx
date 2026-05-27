# SPDX-License-Identifier: MIT OR Apache-2.0
# PsyArcadeGX — canonical player/product executable.
#
# This target deliberately reuses the current crate integration loop while the
# player grows its VFS/cooked-manifest boot path. It is not a sample target:
# it is the stable binary name users and automation should launch.

add_executable(PsyArcadeGX
    ${CMAKE_SOURCE_DIR}/samples/02_crate/main.cpp
)

target_compile_definitions(PsyArcadeGX PRIVATE
    "PSYNDER_GX_APP_LOG_PREFIX=\"PsyArcadeGX\""
    "PSYNDER_GX_APP_WINDOW_TITLE=\"PsyArcadeGX\""
    "PSYNDER_GX_APP_PASS_LABEL=\"arcade player\""
    "PSYNDER_GX_APP_ENTITY_LABEL=\"player_entities\""
    "PSYNDER_GX_UI_IMM_SHADER_PATH=\"${CMAKE_SOURCE_DIR}/engine/ui/imm/shaders/imm_overlay.slang\""
    "PSYNDER_GX_PRIMITIVE_SHADER_PATH=\"${CMAKE_SOURCE_DIR}/samples/02_crate/shaders/primitive_unlit.slang\""
)

target_link_libraries(PsyArcadeGX PRIVATE
    psynder_common
    psynder_core
    psynder_math
    psynder_jobs
    psynder_asset
    psynder_camera
    psynder_scene
    psynder_physics_core
    psynder_script
)

if(TARGET psynder_platform)
    target_link_libraries(PsyArcadeGX PRIVATE psynder_platform)
endif()
if(TARGET psynder_gpu)
    target_link_libraries(PsyArcadeGX PRIVATE psynder_gpu)
endif()
if(TARGET psynder_shader)
    target_link_libraries(PsyArcadeGX PRIVATE psynder_shader)
endif()
if(TARGET psynder_ui_imm)
    target_link_libraries(PsyArcadeGX PRIVATE psynder_ui_imm)
endif()
if(TARGET psynder_editor)
    target_link_libraries(PsyArcadeGX PRIVATE psynder_editor)
else()
    message(FATAL_ERROR "PsyArcadeGX currently requires PSYNDER_GX_EDITOR=ON for runtime console/web IPC")
endif()
