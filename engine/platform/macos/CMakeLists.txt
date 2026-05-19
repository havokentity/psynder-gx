# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_platform_macos — Lane 25.
# Owns: AppKit window, CAMetalLayer surface (lane 07 consumes), IOKit raw
# mouse, GameController gamepad enumeration, CoreAudio device-name lookup
# for lane 14. NSEvent pump runs on the main thread.

# Per-lane option for the standalone surface smoke binary. OFF by default
# so building the engine never accidentally produces a competing main().
option(PSYNDER_GX_PLATFORM_MACOS_BUILD_SMOKE
       "Build sample_macos_smoke (standalone NSWindow + CAMetalLayer test)"
       OFF)

psynder_add_lane(platform_macos)

# psynder_add_lane globs *.cpp / *.mm recursively; MacosSmoke.mm contains
# its own main() and must be excluded from the lane static library or
# every sample binary linking psynder_platform_macos would pull a second
# main symbol. Filter it out post-creation.
if(TARGET psynder_platform_macos)
    get_target_property(_PSY_MACOS_SOURCES psynder_platform_macos SOURCES)
    if(_PSY_MACOS_SOURCES)
        list(FILTER _PSY_MACOS_SOURCES EXCLUDE REGEX ".*MacosSmoke\\.mm$")
        set_property(TARGET psynder_platform_macos PROPERTY SOURCES "${_PSY_MACOS_SOURCES}")
    endif()
endif()

if(PSYNDER_PLATFORM_MACOS)
    # ARC for Objective-C++ TUs. The PsynderGxWindowDelegate +
    # PsynderGxMetalView retain cycles are managed by ARC; our .mm files
    # static_assert __has_feature(objc_arc) so a misconfigured build fails fast.
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/MacosPlatform.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/MacosSmoke.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")

    target_link_libraries(psynder_platform_macos PUBLIC
        "-framework Cocoa"
        "-framework AppKit"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework GameController"
        "-framework IOKit"
        "-framework CoreFoundation"
    )

    # PublicPlatformMacos.h declares ObjC types behind __OBJC__ guards so
    # plain C++ consumers (lane 07's GPU device factory) can include it
    # safely. No additional flags needed for consumers.

    if(PSYNDER_GX_PLATFORM_MACOS_BUILD_SMOKE)
        add_executable(sample_macos_smoke MacosSmoke.mm)
        target_link_libraries(sample_macos_smoke PRIVATE
            psynder_platform_macos
        )
        # The smoke binary needs to see the lane's public header from a
        # plain include path. psynder_platform_macos already exposes
        # ${CMAKE_SOURCE_DIR}/engine on its PUBLIC include path so this
        # is implicit; add an explicit include for safety.
        target_include_directories(sample_macos_smoke PRIVATE
            ${CMAKE_SOURCE_DIR}/engine)
    endif()
endif()
