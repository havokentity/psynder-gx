# SPDX-License-Identifier: MIT OR Apache-2.0
# PsyArcadeGX — canonical player/product executable.
#
# This target deliberately reuses the current crate integration loop while the
# player grows its VFS/cooked-manifest boot path. It is not a sample target:
# it is the stable binary name users and automation should launch.

# PsyArcadeGX currently requires the editor (runtime console + web IPC server).
# It is gated by PSYNDER_GX_BUILD_PLAYER (on by default), independent of the
# editor flag, so a player-on/editor-off configuration (e.g. CI's engine+tests
# matrix, PSYNDER_GX_EDITOR=OFF) reaches here. Skip the target gracefully in that
# case rather than aborting the whole configure with a FATAL_ERROR.
if(NOT TARGET psynder_editor)
    message(STATUS "PsyArcadeGX: skipped (requires PSYNDER_GX_EDITOR=ON for the runtime console / web IPC)")
    return()
endif()

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
    psynder_physics_destruction
    psynder_physics_agents
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
# psynder_editor is guaranteed to exist (the top-of-file guard returned early
# otherwise). PsyArcadeGX links it for the runtime console + web IPC server.
target_link_libraries(PsyArcadeGX PRIVATE psynder_editor)
