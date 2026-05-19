# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_shader — Lane 08.
# Slang shader compiler integration. Emits SPIR-V (for Vulkan) + Metal IR
# (for Apple Silicon). Pipeline state objects (VkPipeline / MTLRenderPipelineState)
# precompiled at build, cached on first run. Filesystem watcher drives hot
# reload between frames.
psynder_add_lane(shader
    LINKS
        psynder_core psynder_jobs psynder_asset psynder_gpu
)
