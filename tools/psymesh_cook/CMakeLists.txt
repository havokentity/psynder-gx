# SPDX-License-Identifier: MIT
# psymesh_cook — Lane 05 offline mesh cooker.
#
# Self-contained: no engine/* link dependencies (this lets the tool
# build standalone and keeps the determinism story simple — the cooker
# uses its own local math + dedup helpers in PsyMeshCook.cpp).
#
# Exposed as a small static library plus a thin CLI driver, so the unit
# tests can call cook() directly without spawning the binary.

# Static lib half — picked up by tests/unit and by the CLI.
add_library(psymesh_cook_lib STATIC
    PsyMeshCook.cpp
)
target_include_directories(psymesh_cook_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(psymesh_cook_lib PRIVATE psynder_common)

# CLI binary half — argv parsing only.
add_executable(psymesh_cook
    main.cpp
)
target_link_libraries(psymesh_cook PRIVATE psymesh_cook_lib psynder_common)
