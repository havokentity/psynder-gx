// SPDX-License-Identifier: MIT
// Psynder(-GX) — Win32 window implementation.
//
// The window class is registered once per process (the class name carries
// the executable pointer so re-using the same binary works), and each
// Win32Window owns its HWND.
//
// In Psynder (!PSYNDER_GX) the window also owns a Win32Present (DXGI swap
// chain + D3D11 blit). In Psynder-GX the present path is removed; present is
// handled by psy::gpu via the HWND exposed through hwnd().

#include "Win32Window.h"

#if defined(PSYNDER_PLATFORM_WIN32)

#include "Win32Input.h"
#include "Win32KeyMap.h"

#include <hidusage.h>
#include <windowsx.h>      // GET_X_LPARAM / GET_Y_LPARAM
#include <vector>          // thread_local raw-input buffer
#include <string>          // std::wstring

namespace psynder::platform::win32 {

namespace {

constexpr wchar_t kWindowClassName[] = L"PsynderWindowClass";

// Tracks raw-input buffer allocations across WM_INPUT callbacks to avoid
// hammering ::GetRawInputData with size queries. The buffer grows monotonic.
thread_local std::vector<u8> g_raw_buf;

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Class registration (one-shot)
// ─────────────────────────────────────────────────────────────────────────

namespace {

ATOM register_window_class(HINSTANCE hinst) {
    static ATOM s_atom = 0;
    if (s_atom != 0) return s_atom;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = &Win32Window::static_wnd_proc;
    wc.hInstance     = hinst;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // engine paints every frame
    wc.lpszClassName = kWindowClassName;

    s_atom = ::RegisterClassExW(&wc);
    if (s_atom == 0) {
        PSY_LOG_ERROR("[win32] RegisterClassExW failed (err={})", ::GetLastError());
    }
    return s_atom;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// ctor / dtor
// ─────────────────────────────────────────────────────────────────────────

Win32Window::Win32Window(const WindowDesc& desc)
    : desc_(desc), window_w_(desc.window_width), window_h_(desc.window_height)
{
    hinstance_ = ::GetModuleHandleW(nullptr);
    if (register_window_class(hinstance_) == 0) return;

    // Adjust the requested client rect to a window rect (account for borders).
    DWORD style     = WS_OVERLAPPEDWINDOW;
    DWORD ex_style  = 0;
    if (!desc.resizable) {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }

    RECT rc{ 0, 0, static_cast<LONG>(desc.window_width),
                  static_cast<LONG>(desc.window_height) };
    ::AdjustWindowRectEx(&rc, style, FALSE, ex_style);

    const std::wstring wtitle = to_wide(desc.title);
    hwnd_ = ::CreateWindowExW(
        ex_style, kWindowClassName, wtitle.c_str(), style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hinstance_, this);
    if (!hwnd_) {
        PSY_LOG_ERROR("[win32] CreateWindowExW failed (err={})", ::GetLastError());
        return;
    }

    // Set the user-data slot to the Win32Window* so the static WndProc can
    // route messages back to the instance. WM_NCCREATE arrives before
    // CreateWindowEx returns, so we also set it in WM_NCCREATE just in case
    // someone calls SetWindowLongPtr before us.
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Pull the actual client size — DPI-aware hosts may have rounded.
    RECT cr{};
    if (::GetClientRect(hwnd_, &cr)) {
        window_w_ = static_cast<u32>(std::max(1L, cr.right  - cr.left));
        window_h_ = static_cast<u32>(std::max(1L, cr.bottom - cr.top));
    }

    if (!register_raw_input()) {
        PSY_LOG_WARN("[win32] raw-input registration failed; mouse deltas fall back to legacy WM_MOUSEMOVE");
    }

#if !defined(PSYNDER_GX)
    // Psynder CPU-renderer path: boot D3D11 DXGI blit present.
    if (!present_.init(hwnd_, desc.render_width, desc.render_height, desc.vsync)) {
        PSY_LOG_ERROR("[win32] DXGI present init failed");
        // Window remains usable for input, just won't render via CPU blit.
    }
#else
    // Psynder-GX: present is owned by psy::gpu (Vulkan swapchain).
    // The HWND is exposed via hwnd() and consumed by psy::gpu::create_device()
    // → Win32VulkanSurface::create_vulkan_surface(). Nothing to init here.
    PSY_LOG_INFO("[win32] GX build: HWND created ({:p}), Vulkan surface creation "
                 "deferred to psy::gpu::create_device()", static_cast<void*>(hwnd_));
#endif

    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);

    // Diagnostic: log the actual window style applied. If the user reports
    // a borderless window despite passing WindowDesc{resizable=true}, the
    // style printed here tells us whether something stripped WS_CAPTION /
    // WS_THICKFRAME between our CreateWindowEx call and ShowWindow.
    const LONG_PTR actual_style    = ::GetWindowLongPtrW(hwnd_, GWL_STYLE);
    const LONG_PTR actual_ex_style = ::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    PSY_LOG_INFO("[win32] window style=0x{:x} ex_style=0x{:x} (WS_OVERLAPPEDWINDOW=0x{:x})",
                 static_cast<std::uint64_t>(actual_style),
                 static_cast<std::uint64_t>(actual_ex_style),
                 static_cast<std::uint64_t>(WS_OVERLAPPEDWINDOW));

    // Frame-sanity log: a correctly-chromed WS_OVERLAPPEDWINDOW reserves a
    // non-client caption (~31px @96dpi) + side/bottom borders, so the client
    // ends up smaller than the window. top/side == 0 means no chrome was
    // drawn — this line makes a future borderless-window regression obvious
    // at startup (see the WM_NCCREATE hwnd_ bind in static_wnd_proc).
    {
        RECT wr{}, cr2{};
        ::GetWindowRect(hwnd_, &wr);
        ::GetClientRect(hwnd_, &cr2);
        POINT tl{0, 0};
        ::ClientToScreen(hwnd_, &tl);
        PSY_LOG_INFO("[win32] client={}x{} non-client frame: top={}px side={}px",
                     static_cast<int>(cr2.right),    static_cast<int>(cr2.bottom),
                     static_cast<int>(tl.y - wr.top), static_cast<int>(tl.x - wr.left));
    }
}

Win32Window::~Win32Window() {
#if !defined(PSYNDER_GX)
    present_.shutdown();
#endif
    if (hwnd_) {
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

Win32Window* create_window_native(const WindowDesc& desc) {
    auto* w = new Win32Window(desc);
    if (!w->valid()) {
        delete w;
        return nullptr;
    }
    return w;
}

// ─────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────

void Win32Window::poll_events() {
    // Reset edge-triggered key flags + sample XInput at frame start.
    input_singleton().begin_frame();

    MSG msg{};
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            should_close_ = true;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

#if !defined(PSYNDER_GX)
    if (size_pending_) {
        present_.on_window_resize(window_w_, window_h_);
        size_pending_ = false;
    }
#endif
}

void Win32Window::present(const render::Framebuffer& fb) {
#if defined(PSYNDER_GX)
    // GX build: present is driven by psy::gpu::end_frame(), not here.
    // If this fires, the game loop is incorrectly calling the CPU-renderer
    // path on a GX build. Suppress the unused-parameter warning.
    (void)fb;
    PSY_LOG_ERROR("[win32] Win32Window::present(fb) called in a GX build. "
                  "Use psy::gpu::end_frame() instead.");
#else
    if (!hwnd_) return;
    present_.present(fb, desc_.scale_mode, desc_.aspect_mode);
#endif
}

void Win32Window::set_title(std::string_view title) {
    desc_.title = std::string{title};
    if (hwnd_) ::SetWindowTextW(hwnd_, to_wide(desc_.title).c_str());
}

// ─────────────────────────────────────────────────────────────────────────
// Raw-input registration (DESIGN §11.1: RegisterRawInputDevices)
// ─────────────────────────────────────────────────────────────────────────

bool Win32Window::register_raw_input() {
    // Register for raw mouse input. RIDEV_NOLEGACY is intentionally NOT set —
    // we want legacy WM_MOUSEMOVE to continue flowing so we still get the
    // absolute cursor position for UI cursor rendering. Raw delta
    // (on_wm_input → on_mouse_raw_delta) provides the unaccelerated
    // high-frequency delta used by the FPS look-around.
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid.usUsage     = HID_USAGE_GENERIC_MOUSE;
    rid.dwFlags     = 0;
    rid.hwndTarget  = hwnd_;
    const BOOL ok = ::RegisterRawInputDevices(&rid, 1, sizeof(rid));
    if (!ok) {
        PSY_LOG_WARN("[win32] RegisterRawInputDevices failed (err={})", ::GetLastError());
    }
    return ok == TRUE;
}

void Win32Window::on_wm_input(LPARAM lparam) {
    UINT cb = 0;
    if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                          nullptr, &cb, sizeof(RAWINPUTHEADER)) != 0) {
        return;
    }
    if (cb == 0) return;
    if (g_raw_buf.size() < cb) g_raw_buf.resize(cb);
    if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                          g_raw_buf.data(), &cb, sizeof(RAWINPUTHEADER)) != cb) {
        return;
    }
    const auto* ri = reinterpret_cast<RAWINPUT*>(g_raw_buf.data());
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = ri->data.mouse;
    // MOUSE_MOVE_ABSOLUTE is set for devices like digitizers that report
    // absolute screen position. For a real mouse it will be zero (relative).
    if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
        input_singleton().on_mouse_raw_delta(
            static_cast<f32>(m.lLastX), static_cast<f32>(m.lLastY));
    }
}

// ─────────────────────────────────────────────────────────────────────────
// WindowProc
// ─────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK Win32Window::static_wnd_proc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NCCREATE) {
        auto* cs   = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = reinterpret_cast<Win32Window*>(cs->lpCreateParams);
        // Bind the real HWND before the first WM_NCCALCSIZE. wnd_proc() routes
        // unhandled messages to DefWindowProcW(hwnd_, ...) using the member
        // handle, but during CreateWindowExW the ctor has not yet assigned
        // hwnd_ (it is set only after the call returns). Without this, the
        // creation-time WM_NCCALCSIZE hit DefWindowProcW(NULL, ...), which
        // reserves no non-client area — the client filled the whole window and
        // no caption/border was drawn (the "borderless window" bug).
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto* self = reinterpret_cast<Win32Window*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->wnd_proc(msg, wparam, lparam);
}

LRESULT Win32Window::wnd_proc(UINT msg, WPARAM wparam, LPARAM lparam) {
    auto& in = input_singleton();
    switch (msg) {
        case WM_CLOSE:
            should_close_ = true;
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        case WM_PAINT: {
            // Engine drives presents from its frame loop; validate the dirty
            // rect so WM_PAINT doesn't keep re-queuing.
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd_, &ps);
            ::EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_SIZE: {
            window_w_ = static_cast<u32>(LOWORD(lparam));
            window_h_ = static_cast<u32>(HIWORD(lparam));
            if (window_w_ == 0) window_w_ = 1;
            if (window_h_ == 0) window_h_ = 1;
#if !defined(PSYNDER_GX)
            // Psynder CPU path defers the ResizeBuffers call to poll_events().
            size_pending_ = true;
#else
            // Psynder-GX: resize_swapchain() is driven by psy::gpu, which
            // must observe the new dimensions on its next begin_frame or on
            // an explicit psynder::gpu::resize_swapchain() call. The game
            // loop is expected to check window_width()/window_height() and
            // call resize_swapchain() when they change.
#endif
            return 0;
        }

        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            const u32  vk    = static_cast<u32>(wparam);
            const u32  flags = static_cast<u32>(lparam);
            const KeyCode k  = vk_to_keycode(vk, flags);
            if (k != KeyCode::Unknown) in.on_key(k, true);
            // Sample / demo convenience: Esc closes the window. Lane 22
            // editor and any production game will want to override this
            // (e.g. open a pause menu). Move to a console var or per-window
            // hook when the input lane lands a proper rebinder.
            if (vk == VK_ESCAPE) {
                should_close_ = true;
            }
            return 0;
        }
        case WM_KEYUP: case WM_SYSKEYUP: {
            const u32 vk    = static_cast<u32>(wparam);
            const u32 flags = static_cast<u32>(lparam);
            const KeyCode k = vk_to_keycode(vk, flags);
            if (k != KeyCode::Unknown) in.on_key(k, false);
            return 0;
        }

        case WM_MOUSEMOVE: {
            const i32 x = GET_X_LPARAM(lparam);
            const i32 y = GET_Y_LPARAM(lparam);
            in.on_mouse_move(static_cast<f32>(x), static_cast<f32>(y));
            return 0;
        }
        case WM_LBUTTONDOWN: in.on_mouse_button(0, true);  return 0;
        case WM_LBUTTONUP:   in.on_mouse_button(0, false); return 0;
        case WM_RBUTTONDOWN: in.on_mouse_button(1, true);  return 0;
        case WM_RBUTTONUP:   in.on_mouse_button(1, false); return 0;
        case WM_MBUTTONDOWN: in.on_mouse_button(2, true);  return 0;
        case WM_MBUTTONUP:   in.on_mouse_button(2, false); return 0;

        case WM_MOUSEWHEEL: {
            const i16 delta = GET_WHEEL_DELTA_WPARAM(wparam);
            in.on_mouse_wheel(static_cast<f32>(delta) / static_cast<f32>(WHEEL_DELTA));
            return 0;
        }

        case WM_INPUT:
            on_wm_input(lparam);
            return ::DefWindowProcW(hwnd_, msg, wparam, lparam);

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wparam, lparam);
}

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
