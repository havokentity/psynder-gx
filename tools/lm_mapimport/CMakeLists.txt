# SPDX-License-Identifier: MIT
# lm_mapimport - one-way Quake/TrenchBroom .map -> .psimport scene importer.
#
# Self-contained: links only psynder_common (engine include root + warning
# flags + C++23). The importer logic is header-only (LmMapImport.h) so the
# unit tests drive it directly and run in the default TOOLS=OFF test build.
# Reuses the shared .map parser in ../lm_qbsp/MapSource.h.
#
# Only built when PSYNDER_GX_BUILD_TOOLS=ON.
add_executable(lm_mapimport main.cpp)
target_link_libraries(lm_mapimport PRIVATE psynder_common)
