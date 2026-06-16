# SPDX-License-Identifier: MIT OR Apache-2.0
# Physics aggregator. Sublanes:
#   core/        — Jolt wrapper, fixed-tick world, character controller (lane 15)
#   vehicle/     — raycast suspension, Pacejka tires, drivetrain         (lane 16)
#   destruction/ — pre-fracture chunks, structural-integrity graph       (lane 17)
#   agents/      — DOTS mass-agent steering on the broadphase  (ADR-019 class 1)
# See DESIGN-PSYNDER-GX.md §10.1.
add_subdirectory(core)
add_subdirectory(vehicle)
add_subdirectory(destruction)
add_subdirectory(agents)

# Convenience union-target.
add_library(psynder_physics INTERFACE)
target_link_libraries(psynder_physics INTERFACE
    psynder_physics_core
    psynder_physics_vehicle
    psynder_physics_destruction
    psynder_physics_agents
)
add_library(psynder::physics ALIAS psynder_physics)
