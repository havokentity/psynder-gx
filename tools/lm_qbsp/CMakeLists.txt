# SPDX-License-Identifier: MIT
# lm_qbsp - Quake-style .map brush set -> engine .psybsp BSP compiler.
#
# Self-contained by design: links only psynder_common (the INTERFACE target
# that supplies the engine include root, C++23, and the project warning
# flags). The compiler logic lives in header-only LmQbsp.h / MapSource.h, so
# the unit tests in tests/unit/ drive it through a relative-path include and
# run even in the default TOOLS=OFF build (psymesh_cook keeps a static lib
# for the same purpose, but our tests can't link a TOOLS-gated lib because
# tests/unit/CMakeLists.txt is orchestrator-owned).
#
# Only built when PSYNDER_GX_BUILD_TOOLS=ON.
add_executable(lm_qbsp main.cpp)
target_link_libraries(lm_qbsp PRIVATE psynder_common)
