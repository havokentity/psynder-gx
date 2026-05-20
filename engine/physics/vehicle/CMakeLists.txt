# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_physics_vehicle — Lane 16.
# Raycast suspension, Pacejka-lite combined-slip tires, drivetrain
# (engine torque curve -> clutch -> gearbox -> diff -> wheels), aero
# drag + downforce. Real metric units throughout (DESIGN §10.1).
psynder_add_lane(physics_vehicle
    LINKS
        psynder_core psynder_math psynder_simd psynder_jobs
        psynder_scene psynder_physics_core
)
# Determinism: physics TUs are built without fast-math + FP contraction so
# replay is bit-stable within a toolchain. clang-cl reports MSVC=ON, so forward
# the same flags to its clang frontend instead of silently skipping them on the
# Windows build (which is clang-cl, not cl.exe).
if(NOT MSVC)
    target_compile_options(psynder_physics_vehicle PRIVATE -fno-fast-math -ffp-contract=off)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(psynder_physics_vehicle PRIVATE
        /clang:-fno-fast-math /clang:-ffp-contract=off)
endif()

# ─── Hook this lane onto the shared unit-test binary ────────────────────
# tests/unit/CMakeLists.txt is owned by lane 25 (tests) and built only when
# PSYNDER_GX_BUILD_TESTS=ON. Defer the link wiring to end-of-configure so we
# attach without editing files outside our lane. Mirrors lane 15 (physics-core).
function(psynder_physics_vehicle_attach_to_unit_target)
    if(TARGET psynder_unit)
        target_link_libraries(psynder_unit PRIVATE psynder_physics_vehicle)
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
    psynder_physics_vehicle_attach_to_unit_target)
