# SPDX-License-Identifier: MIT
# psynder_editor_scene_authoring — loose scene authoring document model/parser.
# Not wired into engine/editor/CMakeLists.txt yet; orchestrator owns that aggregator.
psynder_add_lane(editor_scene_authoring
    LINKS
        psynder_core
        psynder_math
)
