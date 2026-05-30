# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_ai — DOTS combat AI: flow-field pathing now, steering/behaviour to come.
# Deterministic (lockstep), so it builds with the determinism FP flags and is
# covered by the perf_guardrails hot-path lint.
psynder_add_lane(ai
    LINKS
        psynder_core psynder_math
)
if(NOT MSVC)
    target_compile_options(psynder_ai PRIVATE -fno-fast-math -ffp-contract=off)
endif()
