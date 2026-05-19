# SPDX-License-Identifier: MIT OR Apache-2.0
# Offline tools — orchestrator-owned, shared across lanes.
#   lm_pak       — archive tool (shared with Psynder)
#   lm_cook      — base asset cooker; GX adds BC1/BC3/BC7/ASTC + mesh meshlets
#                  (the GPU-specific cooker is lm_cook_gx, to land at M2)
#   lm_qbsp      — BSP compiler (shared with Psynder)
#   lm_bake      — base lightmap baker; GX adds GPU-accelerated probes
#                  (the GPU-accelerated baker is lm_bake_gx, to land at M5)
#   lm_mapimport — Quake .map → engine-ready scene import
#
# Built only when PSYNDER_GX_BUILD_TOOLS is ON.
add_subdirectory(lm_pak)
add_subdirectory(lm_cook)
add_subdirectory(lm_qbsp)
add_subdirectory(lm_bake)
add_subdirectory(lm_mapimport)
