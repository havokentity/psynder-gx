# SPDX-License-Identifier: MIT OR Apache-2.0
# Top-level product executables.

# PsyArcadeGX is the GPU player (requires the editor runtime); it has no place in
# a headless dedicated-server build, which force-disables the editor anyway.
if(NOT PSYNDER_GX_DEDICATED_SERVER)
    add_subdirectory(PsyArcadeGX)
endif()

# PsyServerGX is the headless dedicated server — links only deterministic sim
# lanes, so it builds in both the normal and dedicated configs.
add_subdirectory(PsyServerGX)
