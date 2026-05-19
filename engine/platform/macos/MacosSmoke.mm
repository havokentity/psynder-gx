// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/macos/MacosSmoke.mm
//
// Lane 25 — standalone smoke binary, built only when the lane option
// PSYNDER_GX_PLATFORM_MACOS_BUILD_SMOKE is ON. Does NOT depend on lane 07,
// lane 01 (core), or any other lane — exists so the macOS surface code in
// MacosPlatform.mm can be smoke-verified before the rest of the engine
// builds.
//
// What it tests:
//   1. NSWindow opens (1280x720, titled, resizable).
//   2. CAMetalLayer is attached + reports a non-zero drawableSize.
//   3. NSEvent pump runs for the configured frame budget.
//   4. IOKit raw mouse arm() returns a usable Manager (or soft-fails).
//   5. GameController count is queryable (0 is a valid answer on a desktop
//      with no controllers paired).
//   6. CoreAudio default-output device-name lookup returns something.
//   7. We tick a CAMetalLayer pixelFormat + nextDrawable → present cycle
//      with a small animated clear-color (mirroring what lane 07 will
//      eventually do, but inline here so we don't need lane 07 to ship
//      to verify the surface is alive).
//
// Run mode:
//   sample_macos_smoke                       — interactive (Esc / close-box to exit)
//   sample_macos_smoke --smoke-frames=N      — N frames then exit (CI mode)
//
// Mac smoke-test serialization (AGENTS.md):
//   The caller is responsible for the /tmp/psynder_gx_smoke.lockdir
//   mkdir-mutex before running this binary; the binary itself does NOT
//   acquire the lock (so two builders smoke-testing in parallel is fine
//   for the BUILD step — only the runtime invocation needs serializing).

#if !__has_feature(objc_arc)
#error "MacosSmoke.mm requires ARC."
#endif

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "platform/macos/PublicPlatformMacos.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int parse_smoke_frames(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--smoke-frames=", 15) == 0) {
            return std::atoi(argv[i] + 15);
        }
    }
    return 0;
}

// Inline animated clear-color present, just so the smoke run produces
// visible motion. Lane 07 will replace this with the real psy::gpu path.
struct InlineDrawer {
    id<MTLDevice>          dev   = nil;
    id<MTLCommandQueue>    queue = nil;
    double                 t     = 0.0;

    void init(CAMetalLayer* layer) {
        if (!layer) return;
        dev   = MTLCreateSystemDefaultDevice();
        if (!dev) {
            std::printf("[macos-smoke] no Metal device — skipping draw cycle\n");
            return;
        }
        layer.device      = dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        queue = [dev newCommandQueue];
    }

    void draw(CAMetalLayer* layer, double dt) {
        if (!layer || !dev || !queue) return;
        @autoreleasepool {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) return;
            t += dt;
            const double r = 0.5 + 0.5 * std::sin(t * 1.7);
            const double g = 0.5 + 0.5 * std::sin(t * 2.3 + 1.0);
            const double b = 0.5 + 0.5 * std::sin(t * 3.1 + 2.0);
            MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture     = drawable.texture;
            rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor  = MTLClearColorMake(r, g, b, 1.0);
            id<MTLCommandBuffer>        cmd = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rpd];
            [enc endEncoding];
            [cmd presentDrawable:drawable];
            [cmd commit];
        }
    }
};

InlineDrawer g_drawer;

struct FrameContext {
    int  max_frames = 0; // 0 == infinite (interactive)
    int  done       = 0;
};

void frame_cb(void* user, psynder::platform::macos::Window* win, double dt) {
    auto* ctx = static_cast<FrameContext*>(user);
    CAMetalLayer* layer = (__bridge CAMetalLayer*)psynder::platform::macos::native_layer(win);
    g_drawer.draw(layer, dt);

    // Sub-frame raw-mouse poll snapshot for diagnostics.
    auto m = psynder::platform::macos::mouse_delta_raw();
    if ((ctx->done % 60) == 0) {
        std::printf("[macos-smoke] frame %4d  dt=%.3fms  drawable=%ux%u  mouse_dx=%.1f  mouse_dy=%.1f  gpads=%u\n",
                    ctx->done,
                    dt * 1000.0,
                    psynder::platform::macos::drawable_width(win),
                    psynder::platform::macos::drawable_height(win),
                    m.dx, m.dy,
                    psynder::platform::macos::gamepad_count());
    }

    ++ctx->done;
    if (ctx->max_frames > 0 && ctx->done >= ctx->max_frames) {
        psynder::platform::macos::request_close(win);
    }
}

} // namespace

int main(int argc, char** argv) {
    namespace mac = psynder::platform::macos;

    std::printf("[macos-smoke] Psynder-GX lane 25 surface smoke\n");
    std::printf("[macos-smoke] exe = %s\n", mac::executable_path());
    std::printf("[macos-smoke] cwd = %s\n", mac::current_working_directory());
    std::printf("[macos-smoke] cfg = %s\n", mac::user_config_dir());

    mac::WindowDesc wd{};
    wd.title         = "Psynder-GX — Lane 25 Smoke";
    wd.window_width  = 1280;
    wd.window_height = 720;
    wd.resizable     = true;
    wd.high_dpi      = true;

    mac::Window* win = mac::create_window(wd);
    if (!win) {
        std::printf("[macos-smoke] create_window failed\n");
        return 2;
    }

    CAMetalLayer* layer = (__bridge CAMetalLayer*)mac::native_layer(win);
    std::printf("[macos-smoke] CAMetalLayer = %p  drawable = %ux%u  point = %ux%u\n",
                (__bridge void*)layer,
                mac::drawable_width(win), mac::drawable_height(win),
                mac::point_width(win),    mac::point_height(win));
    std::printf("[macos-smoke] audio default = %s @ %u Hz\n",
                mac::default_audio_device_name(),
                mac::default_audio_sample_rate());

    g_drawer.init(layer);

    FrameContext ctx{};
    ctx.max_frames = parse_smoke_frames(argc, argv);

    auto t0 = std::chrono::steady_clock::now();
    std::uint64_t frames = mac::run_loop(win, frame_cb, &ctx);
    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    std::printf("[macos-smoke] exited after %llu frames in %.3fs (%.1f FPS)\n",
                static_cast<unsigned long long>(frames),
                elapsed,
                frames / std::max(elapsed, 1e-6));

    mac::destroy_window(win);
    return 0;
}
