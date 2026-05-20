# SPDX-License-Identifier: MIT
# lm_bake - offline path-traced lightmap baker (.map -> .lmt).
#
# Self-contained: links only psynder_common (engine include root + warning
# flags + C++23). The tracer logic is header-only (LmBake.h) so the unit
# tests drive it directly and run in the default TOOLS=OFF test build. The
# CLI reuses the shared .map parser in ../lm_qbsp/MapSource.h.
#
# Only built when PSYNDER_GX_BUILD_TOOLS=ON.
add_executable(lm_bake main.cpp)
target_link_libraries(lm_bake PRIVATE psynder_common)
