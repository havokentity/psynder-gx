# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_physics_core — Lane 15.
# Jolt Physics wrapper, fixed 120 Hz tick, character controller (capsule,
# crouch/prone/lean/ladder/water). Deterministic across platforms
# (-fno-fast-math + pinned FP rounding). See DESIGN §10.1.
psynder_add_lane(physics_core
    LINKS
        psynder_core psynder_math psynder_simd psynder_jobs psynder_scene
)

# Determinism: physics TUs compiled without fast-math, per DESIGN §14.
if(NOT MSVC)
    target_compile_options(psynder_physics_core PRIVATE -fno-fast-math -ffp-contract=off)
endif()
# Jolt link wiring happens when third_party/jolt/ is fetched.
# For Wave 0, the lane builds with a stub TU; lane 15 agent fills in
# Jolt-backed rigid body + character controller.
