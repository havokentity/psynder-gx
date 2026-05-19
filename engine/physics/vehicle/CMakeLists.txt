# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_physics_vehicle — Lane 16.
# Raycast/spherecast suspension, Pacejka-lite tires, drivetrain, aero,
# damage, tracked vehicles. M0–M2 ships public-header contract; full
# impl is M7 (DESIGN §13). Real metric units throughout.
psynder_add_lane(physics_vehicle
    LINKS
        psynder_core psynder_math psynder_simd psynder_jobs
        psynder_scene psynder_physics_core
)
if(NOT MSVC)
    target_compile_options(psynder_physics_vehicle PRIVATE -fno-fast-math -ffp-contract=off)
endif()
