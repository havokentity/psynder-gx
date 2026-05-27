# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_ui_imm — legacy target name for Lane 21's GPU overlay system
# (runtime console, boot overlays, gizmos, brush previews, debug HUD,
# perf graphs). New callsites should prefer the psynder_ui_overlay alias.
#
# GX Wave-B: switched from psynder_render_raster to psynder_gpu +
# psynder_shader; overlay geometry is GPU-batched and shader-driven.
psynder_add_lane(ui_imm
    LINKS
        psynder_core
        psynder_math
        psynder_gpu
        psynder_shader
)
add_library(psynder_ui_overlay ALIAS psynder_ui_imm)
add_library(psynder::ui_overlay ALIAS psynder_ui_imm)

target_include_directories(psynder_ui_imm PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/stb")

target_compile_definitions(psynder_ui_imm PRIVATE
    "PSYNDER_GX_UI_OVERLAY_SHADER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/imm_overlay.slang\""
    "PSYNDER_GX_UI_INTRO_SHADER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/runtime_intro.slang\""
    "PSYNDER_GX_UI_OVERLAY_FONT_PATH=\"${CMAKE_SOURCE_DIR}/third_party/fonts/JetBrainsMono/JetBrainsMono-Regular.ttf\""
    "PSYNDER_GX_UI_IMM_SHADER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/imm_overlay.slang\""
    "PSYNDER_GX_UI_IMM_FONT_PATH=\"${CMAKE_SOURCE_DIR}/third_party/fonts/JetBrainsMono/JetBrainsMono-Regular.ttf\"")
