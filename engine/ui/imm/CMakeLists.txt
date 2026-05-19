# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_ui_imm — Lane 21. Owns: immediate-mode in-viewport overlays
# (gizmos, brush previews, debug HUD, perf graphs).
#
# GX Wave-B: switched from psynder_render_raster to psynder_gpu +
# psynder_shader — overlay geometry is now GPU-batched.
psynder_add_lane(ui_imm
    LINKS
        psynder_core
        psynder_math
        psynder_gpu
        psynder_shader
)
