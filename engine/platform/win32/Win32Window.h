// SPDX-License-Identifier: MIT
// Psynder(-GX) — Win32 Window implementation (CreateWindowEx + WndProc).
//
// In Psynder (CPU renderer, !PSYNDER_GX):
//   The window owns a Win32Present (D3D11 DXGI blit path) that uploads a CPU
//   framebuffer to the swap chain each frame.
//
// In Psynder-GX (PSYNDER_GX defined):
//   Presenting is owned by psy::gpu (Vulkan swapchain). Win32Window only
//   manages the HWND + message loop + input registration. The HWND is exposed
//   via hwnd() and passed as DeviceDesc::native_window_handle to
//   psy::gpu::create_device(); Win32VulkanSurface wraps the actual
//   vkCreateWin32SurfaceKHR call. Win32Present is not compiled in GX builds.
//   present(fb) is implemented as a no-op error stub so the class remains
//   concrete (Platform.h Window base has it as pure virtual).

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

#include "platform/Platform.h"
#include "Win32Common.h"

#if !defined(PSYNDER_GX)
// CPU blit path — Psynder only.
#include "Win32Present.h"
#endif

namespace psynder::platform::win32 {

class Win32Window final : public Window {
public:
    explicit Win32Window(const WindowDesc& desc);
    ~Win32Window() override;

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    // ── Public API (Platform.h Window contract) ─────────────────────────
    void poll_events() override;
    bool should_close() const override { return should_close_; }

    // Present a CPU framebuffer.
    //   Non-GX (Psynder): uploads fb to D3D11 and blits to the DXGI swap chain.
    //   GX (Psynder-GX):  no-op stub. In GX the frame loop drives present via
    //                     psy::gpu::end_frame(), not through this API.
    //                     Calling this in a GX build logs an error.
    void present(const render::Framebuffer& fb) override;

    void set_title(std::string_view title) override;
    u32  window_width()  const override { return window_w_; }
    u32  window_height() const override { return window_h_; }

    // True iff the underlying HWND created successfully.
    bool valid() const noexcept { return hwnd_ != nullptr; }

    // Expose the raw HWND so psy::gpu can pass it to Win32VulkanSurface /
    // DeviceDesc::native_window_handle. Only needed by callers that are
    // already inside a PSYNDER_PLATFORM_WIN32 guard.
    HWND hwnd() const noexcept { return hwnd_; }

    // The class-shared WndProc — must be public so Win32Window.cpp's
    // anonymous-namespace `register_window_class()` helper can register it
    // with `RegisterClassExW`. clang-cl + /permissive- is strict about
    // taking the address of a private static member from outside the class
    // (MSVC's permissive mode lets it slide). Dispatches into wnd_proc().
    static LRESULT CALLBACK static_wnd_proc(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    LRESULT wnd_proc(UINT msg, WPARAM wparam, LPARAM lparam);

    // Raw-input registration — one mouse device with RIDEV_NOLEGACY off
    // (we still want the legacy WM_MOUSEMOVE for cursor position).
    bool register_raw_input();

    // Handle WM_INPUT raw-mouse delta packet → forward to Win32Input.
    void on_wm_input(LPARAM lparam);

    // ── State ───────────────────────────────────────────────────────────
    HWND         hwnd_        = nullptr;
    HINSTANCE    hinstance_   = nullptr;
    std::wstring class_name_;

    WindowDesc   desc_;
    u32          window_w_    = 0;
    u32          window_h_    = 0;

    bool         should_close_ = false;

#if !defined(PSYNDER_GX)
    // CPU-renderer present path (Psynder only; absent in GX builds).
    bool         size_pending_ = false;
    Win32Present present_;
#endif
};

// Factory entry — keeps the `new`/`delete` symmetry with `destroy_window_impl`.
Win32Window* create_window_native(const WindowDesc& desc);

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
