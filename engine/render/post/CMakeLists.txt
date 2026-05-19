# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_render_post — Lane 11.
# Owns: bloom, ACES tonemap, vendor-upscaler wrappers (DLSS / FSR / XeSS /
# MetalFX) integrated as thin wrappers in upscale/. UI compositing pass
# integrates via the same target. CPU bloom/scanline/resolve references
# from Psynder live under third_party/psynder-refs/render-post-cpu/.
psynder_add_lane(render_post
    LINKS
        psynder_core psynder_math psynder_simd
        psynder_gpu psynder_shader
)
