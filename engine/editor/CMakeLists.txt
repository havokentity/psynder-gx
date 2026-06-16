# SPDX-License-Identifier: MIT
# Editor aggregator — adds core + ipc; the web/ TypeScript app is built separately by Vite.
add_subdirectory(core)
add_subdirectory(ipc)
add_subdirectory(scene_authoring)

add_library(psynder_editor INTERFACE)
target_link_libraries(psynder_editor INTERFACE
    psynder_editor_core
    psynder_editor_ipc
    psynder_editor_scene_authoring
)
add_library(psynder::editor ALIAS psynder_editor)
