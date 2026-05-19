# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_physics_destruction — Lane 17.
# Pre-fractured chunks (authored offline), structural-integrity graph,
# cascade simulation, debris pool. M0–M2: header contract +
# serializer round-trip; full impl is M7 (DESIGN §10.1, §13).
psynder_add_lane(physics_destruction
    LINKS
        psynder_core psynder_math psynder_jobs psynder_asset
        psynder_scene psynder_physics_core
)
if(NOT MSVC)
    target_compile_options(psynder_physics_destruction PRIVATE -fno-fast-math -ffp-contract=off)
endif()
