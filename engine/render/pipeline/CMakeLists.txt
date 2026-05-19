# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_render_pipeline — Lane 09.
# Owns: forward+ shading, depth pre-pass, HiZ pyramid, GPU-driven culling,
# light cluster build, indirect draw arguments. See DESIGN §7.2–§7.3.

# Per-lane option for a standalone headless smoke binary that exercises
# create / render / destroy without a real GPU device. OFF by default so
# building the engine never produces a competing main().
option(PSYNDER_GX_RENDER_PIPELINE_BUILD_SMOKE
       "Build sample_render_pipeline_smoke (headless API smoke test)"
       OFF)

psynder_add_lane(render_pipeline
    LINKS
        psynder_core psynder_math psynder_simd psynder_jobs
        psynder_scene psynder_asset
        psynder_gpu psynder_shader
)

# Globbing pulls in PipelineSmoke.cpp which defines main(); strip it from
# the lane static library so other binaries linking psynder_render_pipeline
# don't see a duplicate main. Mirrors lane 25's exclusion of MacosSmoke.mm.
if(TARGET psynder_render_pipeline)
    get_target_property(_PSY_RP_SOURCES psynder_render_pipeline SOURCES)
    if(_PSY_RP_SOURCES)
        list(FILTER _PSY_RP_SOURCES EXCLUDE REGEX ".*PipelineSmoke\\.cpp$")
        set_property(TARGET psynder_render_pipeline PROPERTY SOURCES "${_PSY_RP_SOURCES}")
    endif()
endif()

if(PSYNDER_GX_RENDER_PIPELINE_BUILD_SMOKE)
    add_executable(sample_render_pipeline_smoke PipelineSmoke.cpp)
    target_link_libraries(sample_render_pipeline_smoke PRIVATE
        psynder_render_pipeline
    )
    target_include_directories(sample_render_pipeline_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/engine)
endif()
