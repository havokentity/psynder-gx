# SPDX-License-Identifier: MIT
# psynder_platform_win32 — Lane 23. Owns: Win32 window, WASAPI audio, XInput,
# raw mouse input, and (in GX builds) the Vulkan surface creation helper.
#
# In Psynder (!PSYNDER_GX):  also owns Win32Present (D3D11 DXGI CPU blit).
# In Psynder-GX (PSYNDER_GX): Win32Present is excluded; Win32VulkanSurface is
# compiled in its place. psy::gpu (lane 07) owns the Vulkan swapchain; this
# lane only supplies the HWND and the surface.

# psynder_render_raster is linked because Platform.h pulls in render/Framebuffer.h.
# In GX builds render_raster is a stub (the full renderer lives in psy::gpu) but
# the header dependency exists regardless.
psynder_add_lane(platform_win32 LINKS psynder_core psynder_math psynder_render_raster)

if(PSYNDER_PLATFORM_WIN32)
    # ── Core Win32 system libraries (both Psynder and GX) ─────────────────
    target_link_libraries(psynder_platform_win32 PUBLIC
        user32
        gdi32
        shell32
        ole32
        dxgi
        winmm
        xinput
    )

    if(PSYNDER_GX)
        # ── Psynder-GX: Vulkan surface creation ───────────────────────────
        # find_package(Vulkan) is expected to succeed because the root
        # CMakeLists.txt requires Vulkan as part of GX dependencies.
        # Lane 07 (gpu) does the heavy lifting; we only need the headers and
        # the loader import lib to resolve vkGetInstanceProcAddr at link time.
        find_package(Vulkan REQUIRED)
        target_link_libraries(psynder_platform_win32 PUBLIC Vulkan::Vulkan)

        target_sources(psynder_platform_win32 PRIVATE
            Win32VulkanSurface.h
            Win32VulkanSurface.cpp
        )
    else()
        # ── Psynder (CPU renderer): D3D11 + D3DCompiler blit present ──────
        target_link_libraries(psynder_platform_win32 PUBLIC
            d3d11
            d3dcompiler
            d2d1
            dwrite
        )
        target_sources(psynder_platform_win32 PRIVATE
            Win32Present.h
            Win32Present.cpp
        )
    endif()

    target_compile_definitions(psynder_platform_win32 PRIVATE
        UNICODE _UNICODE
    )
endif()
