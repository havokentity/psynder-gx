# SPDX-License-Identifier: MIT OR Apache-2.0
# PsyServerGX — headless dedicated-server executable. No graphics / audio /
# editor: it links only the deterministic headless sim lanes (match -> net +
# gameplay + scene + math + core), so it builds in BOTH the normal player config
# and the dedicated-server config (PSYNDER_GX_DEDICATED_SERVER=ON). This is the
# binary the dedicated-server CI job produces and a headless `--ticks=N` smoke
# exercises.
add_executable(PsyServerGX main.cpp)

target_link_libraries(PsyServerGX PRIVATE
    psynder_common
    psynder_core
    psynder_math
    psynder_scene
    psynder_net
    psynder_gameplay
    psynder_match
)

# Deterministic fixed-tick loop: same strict-FP contract as the sim lanes.
psynder_determinism_fp(PsyServerGX)
