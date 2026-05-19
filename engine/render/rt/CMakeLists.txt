# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_render_rt — Lane 10.
# Owns: hardware ray-traced shadows, DDGI probe grid, hybrid SSR+RT
# reflections. M0–M2: public-header contract + CSM fallback stub.
# Hardware RT impl ships at M5 per DESIGN §7.4–§7.6.
psynder_add_lane(render_rt
    LINKS
        psynder_core psynder_math psynder_simd psynder_jobs
        psynder_scene psynder_gpu psynder_shader
)
