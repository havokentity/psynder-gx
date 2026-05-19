# SPDX-License-Identifier: MIT OR Apache-2.0
# Physics aggregator. Sublanes:
#   core/        — Jolt wrapper, fixed-tick world, character controller (lane 15)
#   vehicle/     — raycast suspension, Pacejka tires, drivetrain         (lane 16)
#   destruction/ — pre-fracture chunks, structural-integrity graph       (lane 17)
# See DESIGN-PSYNDER-GX.md §10.1.
add_subdirectory(core)
add_subdirectory(vehicle)
add_subdirectory(destruction)

# Convenience union-target.
add_library(psynder_physics INTERFACE)
target_link_libraries(psynder_physics INTERFACE
    psynder_physics_core
    psynder_physics_vehicle
    psynder_physics_destruction
)
add_library(psynder::physics ALIAS psynder_physics)
