# SPDX-License-Identifier: MIT OR Apache-2.0
# Render subsystem aggregator (GPU-driven path).
# Sublanes: pipeline (forward+, depth pre-pass, HiZ, GPU cull) /
#          rt (RT shadows, DDGI, hybrid SSR+RT refl) /
#          post (bloom, tonemap, upscale wrappers).
# Lanes 09 / 10 / 11 per PLAN.md §2.
add_subdirectory(raster)   # Wave 0 compat shim; deleted once lanes 12/13/21/23/24/25 refactor
add_subdirectory(pipeline)
add_subdirectory(rt)
add_subdirectory(post)

# Convenience union-target so dependents can just link psynder_render.
add_library(psynder_render INTERFACE)
target_link_libraries(psynder_render INTERFACE
    psynder_render_pipeline
    psynder_render_rt
    psynder_render_post
)
add_library(psynder::render ALIAS psynder_render)
