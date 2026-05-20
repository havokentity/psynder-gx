# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_worldstate — OpaqueWorldState producer (cross-cut slice owned by
# lane 17 physics / destruction). Wraps the wire format that lane 18 lerps
# between when its rewind window straddles two ticks.
#
# Dependencies: psynder_core (Types.h), psynder_jobs (parallel_for above the
# 64-entity threshold). NO link on psynder_net — the integration is one-way:
# lane 18 picks this up via the LagCompAdapter.h header, not the other
# direction. (See AGENTS.md "Public-header contracts" — adding a reverse
# link would create a cycle.)

psynder_add_lane(worldstate
    LINKS psynder_core psynder_jobs
)

# Lockstep determinism (DESIGN §14): the lerp TU MUST be compiled without
# fast-math / fused-multiply-add so the interpolated bytes are bit-identical
# across platforms and compiler versions. Same flags as lane 18 / physics.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(psynder_worldstate PRIVATE
        -fno-fast-math
        -ffp-contract=off
    )
elseif(MSVC)
    target_compile_options(psynder_worldstate PRIVATE /fp:strict)
endif()
