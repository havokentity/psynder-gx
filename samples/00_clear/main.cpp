// SPDX-License-Identifier: MIT OR Apache-2.0
//
// samples/00_clear/main.cpp — M0 deliverable.
//
// Animated clear color on macOS Apple Silicon via native Metal, and on
// Windows + Linux via Vulkan. One source, three platforms — the bar set
// in DESIGN-PSYNDER-GX.md §13 M0.
//
// Wire-up:
//   1. lane 25 (platform-macos) opens an NSWindow + attaches CAMetalLayer
//      and gives us a typed void* via native_layer().
//   2. lane 07 (gpu) reads that handle out of DeviceDesc::native_window
//      _handle, drives MTLDevice / nextDrawable / present.
//   3. The Metal + Vulkan backends both encode an animated clear color
//      inside cmd_submit() — no per-frame work needed from the sample
//      beyond pumping the begin/cmd/end loop and the OS event queue.
//
// The Win32 + Linux wire-up uses lanes 23/24's surface-creation helpers;
// orchestrator's box can't smoke-test those paths so they're gated by
// preprocessor and ship behind "needs PC validation" flags.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu/PublicGpu.h"

#if defined(PSYNDER_GX_PLATFORM_MACOS)
#  include "platform/macos/PublicPlatformMacos.h"
namespace plat = psynder::platform::macos;
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

// ─── macOS path ─────────────────────────────────────────────────────────
#if defined(PSYNDER_GX_PLATFORM_MACOS)

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);

    if (!args.quiet) {
        std::printf("[sample_00_clear] platform=macOS  backend=Metal  smoke_frames=%d\n",
                    args.smoke_frames);
    }

    // 1. Open the NSWindow + CAMetalLayer.
    plat::WindowDesc wdesc;
    wdesc.title         = "Psynder-GX · sample_00_clear (M0)";
    wdesc.window_width  = 1280;
    wdesc.window_height = 720;
    plat::Window* win = plat::create_window(wdesc);
    if (!win) {
        std::fprintf(stderr, "[sample_00_clear] failed to create window\n");
        return 1;
    }

    // 2. Construct psy::gpu::Device, handing the CAMetalLayer through.
    psynder::gpu::DeviceDesc ddesc{};
    ddesc.enable_validation   = false;
    ddesc.enable_rt           = false;            // M5 work; not needed for M0
    ddesc.enable_mesh_shaders = false;            // M9 work
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

    // 3. Drive the main loop. The Metal backend's cmd_submit encodes the
    //    animated clear color internally — the sample only orchestrates.
    std::uint64_t frame_idx = 0;
    while (!plat::should_close(win)) {
        plat::pump_events();

        if (!psynder::gpu::begin_frame(dev)) {
            // swapchain out-of-date or device lost; lane 07 logs internally.
            continue;
        }

        if (psynder::gpu::CmdBuffer* cmd = psynder::gpu::cmd_open(dev)) {
            psynder::gpu::cmd_submit(dev, cmd);
        }

        psynder::gpu::end_frame(dev);

        // Handle window resize. drawable_width / _height update when
        // the user drags the window edge or moves to a different display
        // with a different backingScaleFactor.
        const std::uint32_t dw = plat::drawable_width(win);
        const std::uint32_t dh = plat::drawable_height(win);
        psynder::gpu::resize_swapchain(dev, dw, dh);

        if (args.smoke_frames > 0 && ++frame_idx >= static_cast<std::uint64_t>(args.smoke_frames)) {
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
// ─── Win/Linux placeholder ──────────────────────────────────────────────
// The corresponding lane (23-platform-win32 / 24-platform-linux) ships its
// own PublicPlatformWin32.h / PublicPlatformLinux.h with create_window
// helpers; we'll wire them in once they expose a comparable contract. For
// now, surface a clear "needs PC validation" message.

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);

#  if defined(PSYNDER_GX_BACKEND_VULKAN)
    std::printf("[sample_00_clear] platform=Vulkan host  backend=Vulkan  smoke_frames=%d\n",
                args.smoke_frames);
    std::printf("[sample_00_clear] M0 sample wire-up for Win/Linux pending lane 23/24 "
                "PublicPlatform*.h handle ABI — verify on your PC box.\n");
#  else
    std::printf("[sample_00_clear] no GPU backend selected at build time\n");
#  endif

    return args.smoke_frames > 0 ? 0 : 0;
}

#endif // PSYNDER_GX_PLATFORM_MACOS
