# SPDX-License-Identifier: MIT OR Apache-2.0
#
# engine/platform/linux/CMakeLists.txt
# Lane 24 — Linux platform layer
#   Wayland primary (xdg-shell + vkCreateWaylandSurfaceKHR)
#   X11/XCB fallback (vkCreateXcbSurfaceKHR)
#   evdev raw input
#   Dedicated-server headless build path (PSYNDER_GX_DEDICATED_SERVER=1)
#
# Owned exclusively by lane 24.  Do NOT edit the parent
# engine/platform/CMakeLists.txt (orchestrator-owned).

# ── Source files ─────────────────────────────────────────────────────────
set(LINUX_SOURCES
    LinuxPlatform.cpp
    LinuxCommon.cpp
)

if(NOT PSYNDER_GX_DEDICATED_SERVER)
    list(APPEND LINUX_SOURCES
        LinuxWayland.cpp
        LinuxX11.cpp
        LinuxEvdev.cpp
    )
endif()

# ── Target ───────────────────────────────────────────────────────────────
# psynder_add_lane is a macro defined by cmake/Lanes.cmake (orchestrator-owned).
psynder_add_lane(platform_linux
    SOURCES ${LINUX_SOURCES}
    LINKS   psynder_core psynder_math
)

# ── Platform guard ───────────────────────────────────────────────────────
if(PSYNDER_PLATFORM_LINUX)

    # Compile-time platform macro (belt-and-suspenders — root CMakeLists
    # should already set this, but guard here too).
    target_compile_definitions(psynder_platform_linux PUBLIC
        PSYNDER_PLATFORM_LINUX=1
    )

    # Dedicated-server build strips all windowing / Vulkan-surface code.
    if(PSYNDER_GX_DEDICATED_SERVER)
        target_compile_definitions(psynder_platform_linux PUBLIC
            PSYNDER_GX_DEDICATED_SERVER=1
        )
        # Dedicated server: only syscall / signal / filesystem libraries.
        # No Vulkan, no Wayland, no XCB.
    else()
        # ── Wayland ──────────────────────────────────────────────────────
        # Ubuntu 24.04: apt install libwayland-dev wayland-protocols
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(WAYLAND REQUIRED wayland-client)
        target_include_directories(psynder_platform_linux PRIVATE
            ${WAYLAND_INCLUDE_DIRS}
        )
        target_link_libraries(psynder_platform_linux PUBLIC
            ${WAYLAND_LIBRARIES}
        )

        # xdg-shell generated protocol header.
        # The build system (root CMakeLists) should run wayland-scanner and
        # place the generated header + C source in
        # ${CMAKE_BINARY_DIR}/generated/xdg-shell/.
        # We add the generated include dir here; if the directory is missing,
        # the build will fail with a clear missing-header error rather than a
        # silent wrong-code compile.
        #
        # NEEDS PC VALIDATION: confirm wayland-scanner target exists in root
        # CMakeLists or add a find_program(WAYLAND_SCANNER) here.
        target_include_directories(psynder_platform_linux PRIVATE
            ${CMAKE_BINARY_DIR}/generated/xdg-shell
            ${CMAKE_CURRENT_SOURCE_DIR}  # fallback: hand-placed header
        )

        # ── XCB (X11 fallback) ────────────────────────────────────────────
        # Ubuntu 24.04: apt install libxcb-dev libxcb-icccm4-dev
        pkg_check_modules(XCB REQUIRED xcb)
        target_include_directories(psynder_platform_linux PRIVATE
            ${XCB_INCLUDE_DIRS}
        )
        target_link_libraries(psynder_platform_linux PUBLIC
            ${XCB_LIBRARIES}
        )

        # Optional: xcb-icccm for WM_DELETE_WINDOW atom helpers.
        pkg_check_modules(XCB_ICCCM xcb-icccm)
        if(XCB_ICCCM_FOUND)
            target_link_libraries(psynder_platform_linux PUBLIC
                ${XCB_ICCCM_LIBRARIES}
            )
            target_compile_definitions(psynder_platform_linux PRIVATE
                PSYNDER_HAVE_XCB_ICCCM=1
            )
        endif()

        # ── xkbcommon (keyboard layout on Wayland) ────────────────────────
        # Ubuntu 24.04: apt install libxkbcommon-dev
        pkg_check_modules(XKBCOMMON xkbcommon)
        if(XKBCOMMON_FOUND)
            target_link_libraries(psynder_platform_linux PUBLIC
                ${XKBCOMMON_LIBRARIES}
            )
        endif()

        # ── Vulkan (surface extensions) ────────────────────────────────────
        # VK_KHR_wayland_surface and VK_KHR_xcb_surface are extensions;
        # headers come from vulkan-headers (vcpkg or system vulkan-sdk).
        # The gpu lane (07) owns the VkInstance; we just use the headers.
        find_package(Vulkan REQUIRED)
        target_link_libraries(psynder_platform_linux PUBLIC
            Vulkan::Headers
        )

    endif() # !PSYNDER_GX_DEDICATED_SERVER

endif() # PSYNDER_PLATFORM_LINUX
