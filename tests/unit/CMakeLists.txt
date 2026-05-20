# SPDX-License-Identifier: MIT
# Unit tests. Catch2-based.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(NOT SRC)
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()

if(TARGET Catch2::Catch2WithMain)
    add_executable(psynder_unit ${SRC})
    target_link_libraries(psynder_unit PRIVATE
        Catch2::Catch2WithMain
        psynder_core psynder_math psynder_simd psynder_scene
        psynder_world_bsp psynder_world_outdoor
        psynder_render_raster psynder_render_rt psynder_render_post
        psynder_render_pipeline
        psynder_asset psynder_audio psynder_net psynder_net_voice
        psynder_ui_imm
        psynder_gpu
        psynder_camera
        psynder_worldstate
    )
    # psynder_editor_core is only built when the editor is enabled.
    if(TARGET psynder_editor_core)
        target_link_libraries(psynder_unit PRIVATE psynder_editor_core)
    endif()
    include(Catch)
    catch_discover_tests(psynder_unit)
else()
    add_executable(psynder_unit ${SRC})
    target_link_libraries(psynder_unit PRIVATE
        psynder_core psynder_math psynder_gpu)
    add_test(NAME psynder_unit_smoke COMMAND psynder_unit)
endif()

# Make ctest's non-ASCII Catch2 name filters (e.g. "°", "—") match on Windows: the
# manifest forces the process ANSI code page to UTF-8 so argv is decoded as UTF-8
# rather than the legacy code page. Embedded by the linker via vs_link_exe/mt.
if(MSVC)
    target_sources(psynder_unit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/psynder_unit.manifest")
endif()
