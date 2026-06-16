# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_physics_agents — DOTS mass-agent steering (ADR-019 class 1).
# Thousands of homogeneous steering agents (seek + local avoidance +
# capsule-vs-static) on the scene spatial broadphase, parallel via the job
# system, deterministic for 128-tick lockstep. Agents are ECS entities in
# scene::World (no parallel mutable store).
psynder_add_lane(physics_agents
    LINKS
        psynder_core psynder_math psynder_jobs psynder_scene
)
if(NOT MSVC)
    target_compile_options(psynder_physics_agents PRIVATE -fno-fast-math -ffp-contract=off)
endif()
