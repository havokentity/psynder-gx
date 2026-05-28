# SPDX-License-Identifier: MIT
# Unit tests. Catch2-based.
#
# Suites are globbed (CONFIGURE_DEPENDS) so a new *.cpp is auto-registered on
# the next configure. physics_* suites in this directory:
#   physics_character_controller, physics_core_character_spine,
#   physics_core_smoke, physics_core_smoke_lite, physics_core_waveb_controller,
#   physics_vehicle_waveb_smoke,
#   physics_lagcomp (ADR-020 / #42: hitbox-history rewind / lag comp),
#   destruction_fracture (ADR-021 / #45: deterministic precomputed fracture).
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
        psynder_physics_destruction
    )
    # psynder_editor_core is only built when the editor is enabled.
    if(TARGET psynder_editor_core)
        target_link_libraries(psynder_unit PRIVATE psynder_editor_core)
    endif()
    # psymesh_cook_lib is only built when PSYNDER_GX_BUILD_TOOLS=ON. The
    # tools_psymesh_cook.cpp suite drives the cooker through this library.
    if(TARGET psymesh_cook_lib)
        target_link_libraries(psynder_unit PRIVATE psymesh_cook_lib)
    endif()
    include(Catch)
    catch_discover_tests(psynder_unit)
else()
    add_executable(psynder_unit ${SRC})
    target_link_libraries(psynder_unit PRIVATE
        psynder_core psynder_math psynder_gpu)
    add_test(NAME psynder_unit_smoke COMMAND psynder_unit)
endif()

# ctest passes each discovered Catch2 TEST_CASE name back as a command-line filter.
# On Windows a non-ASCII name (degree sign, em dash, ...) is mangled by the CRT's
# legacy code-page argv decoding, so the filter matches nothing and the test is
# reported as failed even though the product is fine. Keep TEST_CASE names ASCII-only
# (see AGENTS.md) -- that makes discovery robust on every toolchain. An earlier
# activeCodePage=UTF-8 manifest tried to paper over this but silently regressed on
# plain incremental relinks, so it was removed in favour of the naming rule.
