# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_gpu — Lane 07.
# Vulkan + native Metal abstraction. Hides API differences behind
# psy::gpu::{Device, Heap, Buffer, Texture, Cmd, Pipeline, RayTracing,
# Handle<T>}. Implementations under vk/ and mtl/. Per-OS surface
# creation lives in engine/platform/<os>/ and calls into this lane.

psynder_add_lane(gpu
    LINKS
        psynder_core psynder_math psynder_jobs psynder_asset
)

# Per-OS API selection
if(PSYNDER_GX_PLATFORM_MACOS)
    target_compile_definitions(psynder_gpu PUBLIC PSYNDER_GX_BACKEND_METAL=1)
    # Link Metal + QuartzCore at the lane consumer level (platform-macos),
    # not here — keeps psynder_gpu portable as a static lib.
elseif(PSYNDER_GX_PLATFORM_WIN32 OR PSYNDER_GX_PLATFORM_LINUX)
    target_compile_definitions(psynder_gpu PUBLIC PSYNDER_GX_BACKEND_VULKAN=1)
    # Vulkan SDK headers + volk are consumed via vcpkg manifest.
endif()
