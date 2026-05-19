# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_camera — Lane 16 (Camera + Input glue).
#
# Bridges lane 25's per-frame input deltas + lane 02's math into a per-frame
# view / projection matrix the render pipeline can consume. POD CameraState
# is owned by the CALLER, so the API is trivially parallelisable across
# split-screen / multi-view scenarios (drive N CameraStates from N JobSystem
# workers).
psynder_add_lane(camera
    LINKS
        psynder_core psynder_math
)

# Lockstep determinism (DESIGN-PSYNDER-GX.md §14): the camera transform
# feeds frustum culling + gameplay raycasts, so view/proj matrices must be
# bit-identical across hosts. Disable fast-math + fused-MAC on this TU.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(psynder_camera PRIVATE
        -fno-fast-math
        -ffp-contract=off
    )
elseif(MSVC)
    target_compile_options(psynder_camera PRIVATE /fp:strict)
endif()
