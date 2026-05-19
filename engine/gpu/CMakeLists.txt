# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_gpu — Lane 07.
#
# Vulkan + native Metal abstraction. Hides API differences behind
# psy::gpu::{Device, Heap, Buffer, Texture, Cmd, Pipeline, RayTracing,
# Handle<T>}. Implementations:
#
#   mtl/*.mm    — macOS Apple Silicon (Metal + AppKit + QuartzCore)
#   vk/*.cpp    — Win/Linux (volk + Vulkan 1.3)
#   heap/*.cpp  — HeapKind allocators (stubs at M0; full impl M1-M2)
#   cmd/*.cpp   — command-buffer helpers
#
# Per-OS surface creation lives in engine/platform/<os>/ and passes the
# native window handle through DeviceDesc::native_window_handle.

# Per-OS source filtering. The shared psynder_add_lane() helper globs all
# .cpp / .c / .mm under this directory, but it only knows to drop .mm on
# non-macOS. We need to also drop vk/*.cpp on macOS (no Vulkan SDK in the
# default macOS dev env) and mtl/*.mm everywhere except macOS. The helper
# already excludes .mm off-macOS; we still need to exclude vk/ on macOS.
#
# We accomplish this by re-globbing locally and passing the filtered list
# back into a manually-created static lib (psynder_add_lane handles the
# common interface target wiring).

file(GLOB_RECURSE GPU_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.mm"
)

if(PSYNDER_GX_PLATFORM_MACOS)
    # Drop Vulkan TU on macOS — no Vulkan SDK in default toolchain. The
    # VulkanBackend.cpp file is guarded by PSYNDER_GX_BACKEND_VULKAN
    # internally so a stray include would still compile clean, but
    # skipping it speeds the build and avoids any volk header probe.
    list(FILTER GPU_SOURCES EXCLUDE REGEX ".*/vk/.*")
else()
    # Drop Metal sources off-macOS (the lane helper already does this for
    # .mm files via psynder_add_lane, but our manual list-build needs the
    # same filter explicitly).
    list(FILTER GPU_SOURCES EXCLUDE REGEX ".*/mtl/.*")
endif()

# Always have at least one TU so the static lib produces output.
if(GPU_SOURCES STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_psynder_gpu_stub.cpp")
    file(WRITE "${STUB}"
"// SPDX-License-Identifier: MIT\n"
"namespace psynder::gpu_stub { void anchor() {} }\n"
    )
    list(APPEND GPU_SOURCES "${STUB}")
endif()

add_library(psynder_gpu STATIC ${GPU_SOURCES})
add_library(psynder::gpu ALIAS psynder_gpu)

# Inherit the common interface target (warnings, C++23, include paths).
# Only add the common target / sibling lanes if they exist — the lane
# orchestrator always defines them, but worktree-only smoke builds may
# not (the standalone build/lane07-smoke project only adds engine/gpu).
if(TARGET psynder_common)
    target_link_libraries(psynder_gpu PUBLIC psynder_common)
endif()
foreach(_dep psynder_core psynder_math psynder_jobs psynder_asset)
    if(TARGET ${_dep})
        target_link_libraries(psynder_gpu PUBLIC ${_dep})
    endif()
endforeach()

set_target_properties(psynder_gpu PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(psynder_gpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Per-OS API selection + framework / loader wiring
if(PSYNDER_GX_PLATFORM_MACOS)
    target_compile_definitions(psynder_gpu PUBLIC PSYNDER_GX_BACKEND_METAL=1)

    # macOS frameworks needed by the Metal backend. Declared PUBLIC so
    # downstream consumers (samples, platform-macos) inherit the link
    # automatically — matches how Apple's own SDK is consumed. The
    # orchestrator's earlier note (link from platform-macos) holds as a
    # secondary safety net but isn't needed if a sample is the direct
    # consumer.
    find_library(_metal_framework      Metal)
    find_library(_quartzcore_framework QuartzCore)
    find_library(_foundation_framework Foundation)
    find_library(_appkit_framework     AppKit)
    target_link_libraries(psynder_gpu PUBLIC
        ${_metal_framework}
        ${_quartzcore_framework}
        ${_foundation_framework}
        ${_appkit_framework}
    )

    # Objective-C++ source needs ARC off (we manage retains explicitly
    # in MetalBackend.mm) and the modern Obj-C runtime.
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/mtl/MetalBackend.mm
        PROPERTIES
            COMPILE_FLAGS "-fno-objc-arc -fobjc-weak"
    )
elseif(PSYNDER_GX_PLATFORM_WIN32 OR PSYNDER_GX_PLATFORM_LINUX)
    target_compile_definitions(psynder_gpu PUBLIC PSYNDER_GX_BACKEND_VULKAN=1)

    # Vulkan WSI platform protos are gated behind VK_USE_PLATFORM_*_KHR
    # in vulkan/vulkan.h. Without these, symbols like
    # vkCreateWin32SurfaceKHR / vkCreateWaylandSurfaceKHR are not declared
    # and the surface creation in lane 07/23/24 fails to compile.
    if(PSYNDER_GX_PLATFORM_WIN32)
        target_compile_definitions(psynder_gpu PUBLIC VK_USE_PLATFORM_WIN32_KHR=1)
    elseif(PSYNDER_GX_PLATFORM_LINUX)
        # Both because the runtime picks Wayland-vs-X11 dynamically.
        target_compile_definitions(psynder_gpu PUBLIC
            VK_USE_PLATFORM_WAYLAND_KHR=1
            VK_USE_PLATFORM_XCB_KHR=1
            VK_USE_PLATFORM_XLIB_KHR=1
        )
    endif()

    # Vulkan headers — required. Comes from LunarG SDK on Win/Linux. Lets
    # VulkanBackend.h #include <vulkan/vulkan.h> resolve.
    find_package(Vulkan REQUIRED)
    target_link_libraries(psynder_gpu PUBLIC Vulkan::Headers)

    # volk loader — always FetchContent. Why not find_package first:
    # find_package(volk) on some Linux distros finds a different target
    # name than `volk::volk` (or no alias at all), and we end up linking
    # against a nonexistent target. FetchContent is fast (~1500 LOC of C),
    # idempotent across configure runs (cached after first fetch), and
    # gives us a known target name. The user can pre-populate the source
    # by symlinking ./build/<preset>/_deps/volk-src/ to a system clone if
    # they want offline builds.
    include(FetchContent)
    FetchContent_Declare(volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG        1.4.304
        GIT_SHALLOW    TRUE
    )
    # volk needs Vulkan-Headers on its include path before its configure
    # runs. Vulkan::Headers above already provides them.
    FetchContent_MakeAvailable(volk)
    target_link_libraries(psynder_gpu PUBLIC volk)

    if(PSYNDER_GX_PLATFORM_LINUX)
        # Wayland is the primary path per DESIGN §11; XCB fallback.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(_wayland QUIET wayland-client)
            if(_wayland_FOUND)
                target_compile_definitions(psynder_gpu PUBLIC PSYNDER_GX_VK_WAYLAND=1)
                target_include_directories(psynder_gpu PUBLIC ${_wayland_INCLUDE_DIRS})
                target_link_libraries(psynder_gpu PUBLIC ${_wayland_LIBRARIES})
            endif()
            pkg_check_modules(_xcb QUIET xcb)
            if(_xcb_FOUND)
                target_compile_definitions(psynder_gpu PUBLIC PSYNDER_GX_VK_XCB=1)
                target_include_directories(psynder_gpu PUBLIC ${_xcb_INCLUDE_DIRS})
                target_link_libraries(psynder_gpu PUBLIC ${_xcb_LIBRARIES})
            endif()
        endif()
    endif()
endif()
