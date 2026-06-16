# SPDX-License-Identifier: MIT
# Perf / DOTS / determinism guardrails as a ctest. Runs scripts/perf_guardrails.sh
# (DOTS hot-path discipline + determinism-flag enforcement) on every `ctest`
# invocation and in CI, so a regression fails loud. See the script header +
# docs/AUTONOMOUS-CHARTER.md. bash is present on the macOS + Linux test runners
# (Windows CI builds only; the determinism matrix covers it separately).
add_test(NAME perf_guardrails
         COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/perf_guardrails.sh")
