// SPDX-License-Identifier: MIT OR Apache-2.0
//
// samples/00_clear/main.cpp — M0 deliverable.
//
// Animated clear color on macOS Apple Silicon via native Metal, and on
// Windows via Vulkan. One source, multiple platforms — the bar set in
// DESIGN-PSYNDER-GX.md §13 M0.
//
// Wire-up:
//   * macOS: lane 25 opens an NSWindow + CAMetalLayer; lane 07 drives
//     MTLDevice / nextDrawable / present.
//   * Windows: lane 23 opens a Win32 HWND; lane 07 wraps it via
//     vkCreateWin32SurfaceKHR + drives the swapchain.
//   * Linux: pending — lane 24 has Wayland / XCB surface helpers but
//     they need a tagged-handle ABI agreement with lane 07 before the
//     sample can hand off a void* unambiguously. Issue filed in lane
//     09's INTEGRATION.txt (lane09-002 / lane24-handle-abi).
//
// On Metal AND Vulkan the backend's cmd_submit encodes an animated
// clear color internally; the sample only orchestrates the begin /
// cmd / end loop and the OS event pump.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu/PublicGpu.h"

#if defined(PSYNDER_GX_PLATFORM_MACOS)
#  include "platform/macos/PublicPlatformMacos.h"
#elif defined(PSYNDER_GX_PLATFORM_WIN32)
#  include "platform/Platform.h"
#  include "platform/win32/Win32Window.h"
#elif defined(PSYNDER_GX_PLATFORM_LINUX)
#  include "platform/Platform.h"
#  include "platform/linux/PublicPlatformLinux.h"
#endif

namespace {

struct Args {
    int  smoke_frames = 0; // > 0 = run N frames then exit (CI mode)
    bool quiet        = false;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--smoke-frames=", 15) == 0) {
            a.smoke_frames = std::atoi(argv[i] + 15);
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            a.quiet = true;
        }
    }
    return a;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────
// macOS — native Metal via lane 25
// ────────────────────────────────────────────────────────────────────────
#if defined(PSYNDER_GX_PLATFORM_MACOS)

int main(int argc, char** argv) {
    namespace plat = psynder::platform::macos;
    const Args args = parse(argc, argv);

    if (!args.quiet) {
        std::printf("[sample_00_clear] platform=macOS  backend=Metal  smoke_frames=%d\n",
                    args.smoke_frames);
    }

    plat::WindowDesc wdesc;
    wdesc.title         = "Psynder-GX · sample_00_clear (M0)";
    wdesc.window_width  = 1280;
    wdesc.window_height = 720;
    plat::Window* win = plat::create_window(wdesc);
    if (!win) {
        std::fprintf(stderr, "[sample_00_clear] failed to create window\n");
        return 1;
    }

    psynder::gpu::DeviceDesc ddesc{};
    ddesc.enable_validation    = false;
    ddesc.enable_rt            = false;
    ddesc.enable_mesh_shaders  = false;
    ddesc.native_window_handle = plat::native_layer(win);

    psynder::gpu::Device* dev = psynder::gpu::create_device(ddesc);
    if (!dev) {
        std::fprintf(stderr, "[sample_00_clear] psy::gpu::create_device failed\n");
        plat::destroy_window(win);
        return 1;
    }
    if (!args.quiet) {
        std::printf("[sample_00_clear] device=%s  unified_memory=%d  rt=%d  mesh=%d\n",
                    psynder::gpu::device_name(dev),
                    psynder::gpu::device_is_unified_memory(dev) ? 1 : 0,
                    psynder::gpu::device_supports_rt(dev) ? 1 : 0,
                    psynder::gpu::device_supports_mesh_shaders(dev) ? 1 : 0);
    }

    std::uint64_t frame_idx = 0;
    while (!plat::should_close(win)) {
        plat::pump_events();
        if (!psynder::gpu::begin_frame(dev)) continue;
        if (auto* cmd = psynder::gpu::cmd_open(dev)) {
            psynder::gpu::cmd_submit(dev, cmd);
        }
        psynder::gpu::end_frame(dev);
        psynder::gpu::resize_swapchain(dev,
            plat::drawable_width(win),
            plat::drawable_height(win));
        if (args.smoke_frames > 0 &&
            ++frame_idx >= static_cast<std::uint64_t>(args.smoke_frames)) {
            if (!args.quiet) {
                std::printf("[sample_00_clear] smoke complete: %llu frames\n",
                            static_cast<unsigned long long>(frame_idx));
            }
            break;
        }
    }

    psynder::gpu::destroy_device(dev);
    plat::destroy_window(win);
    return 0;
}

// ────────────────────────────────────────────────────────────────────────
// Windows — Vulkan swapchain on a Win32 HWND via lane 23 + lane 07
// ────────────────────────────────────────────────────────────────────────
#elif defined(PSYNDER_GX_PLATFORM_WIN32)

int main(int argc, char** argv) {
    namespace plat = psynder::platform;
    const Args args = parse(argc, argv);

    if (!args.quiet) {
        std::printf("[sample_00_clear] platform=Windows  backend=Vulkan  smoke_frames=%d\n",
                    args.smoke_frames);
        std::fflush(stdout); // so the user sees this even if the GPU init crashes
    }

    plat::WindowDesc wdesc;
    wdesc.title         = "Psynder-GX · sample_00_clear (M0)";
    wdesc.window_width  = 1280;
    wdesc.window_height = 720;

    plat::Window* win = plat::create_window(wdesc);
    if (!win) {
        std::fprintf(stderr, "[sample_00_clear] failed to create Win32 window\n");
        return 1;
    }

    // The cross-platform create_window() returns Window*. On Win32 the
    // concrete type is psynder::platform::win32::Win32Window; downcast
    // so we can grab the raw HWND for psy::gpu::DeviceDesc.
    auto* w32 = static_cast<plat::win32::Win32Window*>(win);
    HWND hwnd = w32->hwnd();
    if (!hwnd) {
        std::fprintf(stderr, "[sample_00_clear] Win32Window returned null HWND\n");
        plat::destroy_window(win);
        return 1;
    }

    psynder::gpu::DeviceDesc ddesc{};
    ddesc.enable_validation    = false;
    ddesc.enable_rt            = false;
    ddesc.enable_mesh_shaders  = false;
    ddesc.native_window_handle = reinterpret_cast<void*>(hwnd);

    psynder::gpu::Device* dev = psynder::gpu::create_device(ddesc);
    if (!dev) {
        std::fprintf(stderr, "[sample_00_clear] psy::gpu::create_device failed (check Vulkan runtime + ICD)\n");
        plat::destroy_window(win);
        return 1;
    }
    if (!args.quiet) {
        std::printf("[sample_00_clear] device=%s  rt=%d  mesh=%d\n",
                    psynder::gpu::device_name(dev),
                    psynder::gpu::device_supports_rt(dev) ? 1 : 0,
                    psynder::gpu::device_supports_mesh_shaders(dev) ? 1 : 0);
        std::fflush(stdout);
    }

    std::uint64_t frame_idx = 0;
    while (!win->should_close()) {
        win->poll_events();
        if (!psynder::gpu::begin_frame(dev)) continue;
        if (auto* cmd = psynder::gpu::cmd_open(dev)) {
            psynder::gpu::cmd_submit(dev, cmd);
        }
        psynder::gpu::end_frame(dev);
        psynder::gpu::resize_swapchain(dev,
            win->window_width(),
            win->window_height());
        if (args.smoke_frames > 0 &&
            ++frame_idx >= static_cast<std::uint64_t>(args.smoke_frames)) {
            if (!args.quiet) {
                std::printf("[sample_00_clear] smoke complete: %llu frames\n",
                            static_cast<unsigned long long>(frame_idx));
            }
            break;
        }
    }

    psynder::gpu::destroy_device(dev);
    plat::destroy_window(win);
    return 0;
}

// ────────────────────────────────────────────────────────────────────────
// Linux — Vulkan swapchain on Wayland or X11/XCB via lane 24 + lane 07
// ────────────────────────────────────────────────────────────────────────
#elif defined(PSYNDER_GX_PLATFORM_LINUX)

int main(int argc, char** argv) {
    namespace plat      = psynder::platform;
    namespace lin_plat  = psynder::platform::linux_platform;
    const Args args = parse(argc, argv);

    if (!args.quiet) {
        std::printf("[sample_00_clear] platform=Linux  backend=Vulkan  smoke_frames=%d\n",
                    args.smoke_frames);
        std::fflush(stdout);
    }

    plat::WindowDesc wdesc;
    wdesc.title         = "Psynder-GX · sample_00_clear (M0)";
    wdesc.window_width  = 1280;
    wdesc.window_height = 720;

    plat::Window* win = plat::create_window(wdesc);
    if (!win) {
        std::fprintf(stderr, "[sample_00_clear] failed to create Linux window "
                              "(Wayland / XCB both unavailable)\n");
        return 1;
    }

    // Obtain the typed handle (LinuxNativeWindowHandle*) for the Vulkan backend.
    // Lane 24's create_window_impl populated this when it created the window.
    const auto* lh = lin_plat::native_window_handle(win);
    if (!lh) {
        std::fprintf(stderr,
            "[sample_00_clear] native_window_handle returned null — "
            "window is not a graphical type or was not created by lane 24\n");
        plat::destroy_window(win);
        return 1;
    }

    psynder::gpu::DeviceDesc ddesc{};
    ddesc.enable_validation    = false;
    ddesc.enable_rt            = false;
    ddesc.enable_mesh_shaders  = false;
    // Cast LinuxNativeWindowHandle* to void* for DeviceDesc::native_window_handle.
    // VulkanBackend::create_surface_() casts it back on the Linux path.
    ddesc.native_window_handle = const_cast<psynder::gpu::LinuxNativeWindowHandle*>(lh);

    psynder::gpu::Device* dev = psynder::gpu::create_device(ddesc);
    if (!dev) {
        std::fprintf(stderr,
            "[sample_00_clear] psy::gpu::create_device failed "
            "(check Vulkan runtime + ICD + Wayland/XCB WSI extension)\n");
        plat::destroy_window(win);
        return 1;
    }
    if (!args.quiet) {
        std::printf("[sample_00_clear] device=%s  rt=%d  mesh=%d\n",
                    psynder::gpu::device_name(dev),
                    psynder::gpu::device_supports_rt(dev) ? 1 : 0,
                    psynder::gpu::device_supports_mesh_shaders(dev) ? 1 : 0);
        std::fflush(stdout);
    }

    std::uint64_t frame_idx = 0;
    while (!win->should_close()) {
        win->poll_events();
        if (!psynder::gpu::begin_frame(dev)) continue;
        if (auto* cmd = psynder::gpu::cmd_open(dev)) {
            psynder::gpu::cmd_submit(dev, cmd);
        }
        psynder::gpu::end_frame(dev);
        psynder::gpu::resize_swapchain(dev,
            win->window_width(),
            win->window_height());
        if (args.smoke_frames > 0 &&
            ++frame_idx >= static_cast<std::uint64_t>(args.smoke_frames)) {
            if (!args.quiet) {
                std::printf("[sample_00_clear] smoke complete: %llu frames\n",
                            static_cast<unsigned long long>(frame_idx));
            }
            break;
        }
    }

    psynder::gpu::destroy_device(dev);
    plat::destroy_window(win);
    return 0;
}

#else

int main(int /*argc*/, char** /*argv*/) {
    std::printf("[sample_00_clear] no GPU backend selected at build time\n");
    return 1;
}

#endif
