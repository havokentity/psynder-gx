# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_gameplay — FPS gameplay systems (health / armor / damage / respawn,
# and — as they land — weapons, rounds, pickups). Deterministic, on the
# authoritative lockstep tick, so it builds with the determinism FP flags and is
# covered by the perf_guardrails hot-path lint.
psynder_add_lane(gameplay
    LINKS
        psynder_core psynder_math psynder_scene psynder_ai
)
if(NOT MSVC)
    target_compile_options(psynder_gameplay PRIVATE -fno-fast-math -ffp-contract=off)
endif()
