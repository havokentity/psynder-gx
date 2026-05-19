# SPDX-License-Identifier: MIT OR Apache-2.0
# Wave 0 compatibility shim — header-only. Lane agents in 12/13/21 + the
# platform lanes 23/24/25 will refactor callers off
# psynder::render::raster::* onto psy::gpu / psy::render::pipeline,
# after which this directory can be deleted. The target name
# `psynder_render_raster` is preserved to keep vendored Psynder
# CMakeLists.txt working without edits during Wave 0.
add_library(psynder_render_raster INTERFACE)
add_library(psynder::render_raster ALIAS psynder_render_raster)
target_include_directories(psynder_render_raster INTERFACE ${CMAKE_SOURCE_DIR}/engine)
