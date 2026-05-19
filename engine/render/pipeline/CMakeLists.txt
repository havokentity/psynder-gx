# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_render_pipeline — Lane 09.
# Owns: forward+ shading, depth pre-pass, HiZ pyramid, GPU-driven culling,
# light cluster build, indirect draw arguments. See DESIGN §7.2–§7.3.
psynder_add_lane(render_pipeline
    LINKS
        psynder_core psynder_math psynder_simd psynder_jobs
        psynder_scene psynder_asset
        psynder_gpu psynder_shader
)
