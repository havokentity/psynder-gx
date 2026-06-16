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
#include <string>
#include <string_view>

#include "core/console/RuntimeConsole.h"
#include "gpu/PublicGpu.h"
#include "platform/Platform.h"
#include "ui/imm/RuntimeConsoleGpu.h"

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

std::string gpu_info_text(psynder::gpu::Device* dev, const char* backend) {
    char buf[384]{};
    std::snprintf(buf,
                  sizeof(buf),
                  "backend: %s\n"
                  "device: %s\n"
                  "unified_memory: %s\n"
                  "raytracing: %s\n"
                  "mesh_shaders: %s\n",
                  backend,
                  psynder::gpu::device_name(dev),
                  psynder::gpu::device_is_unified_memory(dev) ? "yes" : "no",
                  psynder::gpu::device_supports_rt(dev) ? "yes" : "no",
                  psynder::gpu::device_supports_mesh_shaders(dev) ? "yes" : "no");
    return std::string{buf};
}

void encode_console_overlay_clear(psynder::gpu::CmdBuffer* cmd,
                                  psynder::ui::imm::RuntimeConsoleGpu& overlay,
                                  std::uint32_t drawable_width,
                                  std::uint32_t drawable_height,
                                  std::uint32_t point_width,
                                  std::uint32_t point_height) {
    namespace g = psynder::gpu;
    g::RenderPassDesc pass{};
    pass.color_count = 1;
    pass.colors[0].target = nullptr;
    pass.colors[0].load = g::LoadOp::Clear;
    pass.colors[0].store = g::StoreOp::Store;
    pass.colors[0].clear_rgba[0] = 0.05f;
    pass.colors[0].clear_rgba[1] = 0.07f;
    pass.colors[0].clear_rgba[2] = 0.10f;
    pass.colors[0].clear_rgba[3] = 1.0f;
    pass.depth.target = nullptr;
    pass.swapchain = true;

    g::begin_render(cmd, pass);
    g::set_viewport(cmd, g::Viewport{
        0.0f, 0.0f,
        static_cast<float>(drawable_width),
        static_cast<float>(drawable_height),
        0.0f, 1.0f});
    g::set_scissor(cmd, g::Scissor{
        0,
        0,
        drawable_width,
        drawable_height});
    psynder::ui::imm::draw_runtime_console_gpu(cmd,
                                               overlay,
                                               static_cast<float>(point_width),
                                               static_cast<float>(point_height));
    g::end_render(cmd);
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
    psynder::console::init_runtime_console();
    psynder::console::set_runtime_console_clipboard_setter([](std::string_view text) {
        const std::string copy{text};
        plat::set_clipboard_text(copy.c_str());
    });
    psynder::console::set_runtime_console_gpu_info_provider([dev] {
        return gpu_info_text(dev, "Metal");
    });
    psynder::console::set_runtime_console_render_stats_provider([&frame_idx] {
        char buf[128]{};
        std::snprintf(buf, sizeof(buf), "frames: %llu\npass: clear\n",
                      static_cast<unsigned long long>(frame_idx));
        return std::string{buf};
    });
    psynder::ui::imm::RuntimeConsoleGpu console_overlay{};
    const bool console_overlay_ok =
        psynder::ui::imm::init_runtime_console_gpu(dev, console_overlay);
    if (!console_overlay_ok && !args.quiet) {
        std::fputs("[sample_00_clear] runtime console overlay disabled\n", stderr);
    }

    while (!plat::should_close(win)) {
        plat::pump_events();
        auto* input = psynder::platform::input();
        const bool toggle_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Tilde);
        const bool escape_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Escape);
        const bool enter_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Enter);
        const bool backspace_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Backspace);
        const bool history_prev_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Up);
        const bool history_next_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Down);
        const bool backspace_down =
            input && input->key_down(psynder::platform::KeyCode::Backspace);
        const bool history_prev_down =
            input && input->key_down(psynder::platform::KeyCode::Up);
        const bool history_next_down =
            input && input->key_down(psynder::platform::KeyCode::Down);
        const bool edit_left_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Left);
        const bool edit_right_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Right);
        const bool edit_left_down =
            input && input->key_down(psynder::platform::KeyCode::Left);
        const bool edit_right_down =
            input && input->key_down(psynder::platform::KeyCode::Right);
        const bool edit_home_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Home);
        const bool edit_end_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::End);
        const bool edit_delete_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Delete);
        const bool shift_down =
            input && (input->key_down(psynder::platform::KeyCode::LeftShift) ||
                      input->key_down(psynder::platform::KeyCode::RightShift));
        const bool tab_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Tab);
        const psynder::platform::MouseState mouse =
            input ? input->mouse() : psynder::platform::MouseState{};
        const float console_scroll = mouse.wheel;
        char console_text[128]{};
        const std::size_t console_text_len =
            plat::text_input_utf8(console_text, sizeof(console_text));
        const auto console_shortcuts = plat::consume_console_shortcuts();
        psynder::console::update_runtime_console(
            psynder::console::RuntimeConsoleInput{
                toggle_pressed,
                escape_pressed,
                enter_pressed,
                backspace_pressed,
                history_prev_pressed,
                history_next_pressed,
                backspace_down,
                history_prev_down,
                history_next_down,
                edit_left_pressed,
                edit_right_pressed,
                edit_left_down,
                edit_right_down,
                edit_home_pressed,
                edit_end_pressed,
                edit_delete_pressed,
                shift_down,
                console_shortcuts.copy,
                console_shortcuts.cut,
                console_shortcuts.paste,
                console_shortcuts.select_all,
                tab_pressed,
                console_scroll,
                mouse.x,
                mouse.y,
                mouse.left,
                static_cast<float>(plat::point_width(win)),
                static_cast<float>(plat::point_height(win)),
                std::string_view{console_shortcuts.paste_text ?
                                     console_shortcuts.paste_text : ""},
                std::string_view{console_text, console_text_len}});
        psynder::console::pump_runtime_console();
        if (psynder::console::consume_runtime_console_quit_requested()) {
            plat::request_close(win);
        }
        if (!psynder::gpu::begin_frame(dev)) {
            // begin_frame can fail on OUT_OF_DATE_KHR / similar Metal surface
            // events (window resize, display change). Drive a swapchain
            // resize before retrying — otherwise the loop spins on stale
            // surface caps forever and the window appears frozen.
            psynder::gpu::resize_swapchain(dev,
                plat::drawable_width(win),
                plat::drawable_height(win));
            continue;
        }
        if (auto* cmd = psynder::gpu::cmd_open(dev)) {
            if (console_overlay_ok &&
                psynder::ui::imm::runtime_console_gpu_wants_draw(console_overlay)) {
                encode_console_overlay_clear(cmd,
                                             console_overlay,
                                             plat::drawable_width(win),
                                             plat::drawable_height(win),
                                             plat::point_width(win),
                                             plat::point_height(win));
            }
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

    psynder::ui::imm::shutdown_runtime_console_gpu(console_overlay);
    psynder::console::set_runtime_console_clipboard_setter({});
    psynder::console::set_runtime_console_gpu_info_provider({});
    psynder::console::set_runtime_console_render_stats_provider({});
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
        if (!psynder::gpu::begin_frame(dev)) {
            // begin_frame can fail on OUT_OF_DATE_KHR (window resize, monitor
            // mode change, sleep/wake). Drive a swapchain resize before
            // retrying — otherwise the loop spins on stale surface caps
            // forever and the window appears frozen.
            psynder::gpu::resize_swapchain(dev,
                win->window_width(),
                win->window_height());
            continue;
        }
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
        if (!psynder::gpu::begin_frame(dev)) {
            // begin_frame can fail on OUT_OF_DATE_KHR (window resize, monitor
            // mode change, sleep/wake). Drive a swapchain resize before
            // retrying — otherwise the loop spins on stale surface caps
            // forever and the window appears frozen.
            psynder::gpu::resize_swapchain(dev,
                win->window_width(),
                win->window_height());
            continue;
        }
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
