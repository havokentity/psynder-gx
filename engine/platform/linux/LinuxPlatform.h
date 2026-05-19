// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/linux/LinuxPlatform.h
//
// Lane 24 — Linux platform internal header.
// Wayland primary (xdg-shell + vkCreateWaylandSurfaceKHR),
// X11 fallback (XCB + vkCreateXcbSurfaceKHR), evdev raw input,
// and headless/dedicated-server build path.
//
// All declarations are guarded by #if PSYNDER_PLATFORM_LINUX.
// See DESIGN-PSYNDER-GX.md §11.2 + §11.4.

#pragma once

#if PSYNDER_PLATFORM_LINUX

#include <cstdint>
#include <string>

// Vulkan surface extension headers (Vulkan SDK or system vulkan-headers).
// Include only where PSYNDER_GX_DEDICATED_SERVER is NOT set.
#ifndef PSYNDER_GX_DEDICATED_SERVER

#include <vulkan/vulkan.h>

// Wayland client — libwayland-dev (Ubuntu 24.04) or vendored.
#include <wayland-client.h>

// XCB — libxcb-dev (preferred over Xlib; thread-safe per DESIGN §11.2).
#include <xcb/xcb.h>

#endif // !PSYNDER_GX_DEDICATED_SERVER

namespace psynder::platform::linux_platform {

// ─── Session type detection ───────────────────────────────────────────────
// Returns true when WAYLAND_DISPLAY env var is set OR XDG_SESSION_TYPE==wayland.
// Call once at startup; result is stable for process lifetime.
bool is_wayland_session() noexcept;

// ─── Vulkan surface creation ─────────────────────────────────────────────
// These are defined only when not building a dedicated server.

#ifndef PSYNDER_GX_DEDICATED_SERVER

/// Create a VkSurfaceKHR for a Wayland wl_surface.
/// Returns VK_NULL_HANDLE on failure (check vkResult via out_result).
/// Caller owns the returned VkSurfaceKHR and must destroy it via
/// vkDestroySurfaceKHR before destroying the VkInstance.
VkSurfaceKHR create_vulkan_surface_wayland(
    wl_display*  display,
    wl_surface*  surface,
    VkInstance   instance,
    VkResult*    out_result = nullptr);

/// Create a VkSurfaceKHR for an XCB window.
/// Prefer over the Xlib path for thread-safety (DESIGN §11.2).
VkSurfaceKHR create_vulkan_surface_xcb(
    xcb_connection_t* connection,
    xcb_window_t      window,
    VkInstance        instance,
    VkResult*         out_result = nullptr);

// ─── Opaque window handles ────────────────────────────────────────────────

struct WaylandWindowHandle {
    wl_display*  display  = nullptr;
    wl_surface*  surface  = nullptr;
    // xdg_surface and xdg_toplevel are void* to avoid pulling xdg-shell
    // headers into every translation unit that includes this header.
    void* xdg_surface   = nullptr; // xdg_surface*
    void* xdg_toplevel  = nullptr; // xdg_toplevel*
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    bool should_close    = false;
    bool configured      = false;  // set after first xdg_surface::configure
};

struct XcbWindowHandle {
    xcb_connection_t* connection = nullptr;
    xcb_window_t      window     = 0;
    xcb_atom_t        wm_delete_window = 0;
    std::uint32_t     width  = 0;
    std::uint32_t     height = 0;
    bool should_close = false;
};

// ─── Window creation / destruction ───────────────────────────────────────

struct WindowCreateInfo {
    const char*   title       = "Psynder-GX";
    std::uint32_t width       = 1280;
    std::uint32_t height      = 720;
    bool          fullscreen  = false;
};

/// Create a Wayland window (xdg-shell toplevel).
/// Caller must eventually call destroy_wayland_window().
WaylandWindowHandle* create_wayland_window(const WindowCreateInfo& info);
void                 destroy_wayland_window(WaylandWindowHandle* wnd);

/// Pump Wayland events. Sets wnd->should_close on close request.
void poll_wayland_events(WaylandWindowHandle* wnd);

/// Create an XCB window (X11 fallback).
XcbWindowHandle* create_xcb_window(const WindowCreateInfo& info);
void             destroy_xcb_window(XcbWindowHandle* wnd);

/// Pump XCB events. Sets wnd->should_close on WM_DELETE_WINDOW.
void poll_xcb_events(XcbWindowHandle* wnd);

#endif // !PSYNDER_GX_DEDICATED_SERVER

// ─── evdev raw mouse input ────────────────────────────────────────────────
// Works in both graphical and dedicated-server builds (server may still want
// console keyboard input via evdev, but the raw mouse part is guarded below).

/// Opaque evdev reader. Reads from /dev/input/event* files.
struct EvdevReader;

/// Open all EV_REL + EV_KEY event devices found under /dev/input/.
/// Caller must eventually call evdev_close().
/// NOTE: Needs read access to /dev/input/event* (udev rule or input group).
EvdevReader* evdev_open();
void         evdev_close(EvdevReader* reader);

struct RawMouseDelta {
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    std::int32_t wheel = 0; // REL_WHEEL ticks
};

/// Poll all open evdev devices and accumulate deltas since last call.
/// Non-blocking; returns immediately with zeros if no events are pending.
RawMouseDelta evdev_poll_mouse(EvdevReader* reader);

/// Returns true if a key is currently held (Linux key code, not a KeyCode enum).
bool evdev_key_held(EvdevReader* reader, std::uint16_t linux_key_code);

// ─── Process / signal utilities (available in all build modes) ───────────

/// Install SIGTERM + SIGINT handlers that set a global shutdown flag.
/// Use psynder_linux_should_quit() to check.
void install_signal_handlers();
bool should_quit() noexcept;

// ─── Filesystem helpers (available in all build modes) ────────────────────
std::string executable_path();
std::string user_config_dir();  // $XDG_CONFIG_HOME/psynder-gx or ~/.config/psynder-gx
std::string user_data_dir();    // $XDG_DATA_HOME/psynder-gx or ~/.local/share/psynder-gx

// ─── Monotonic clock (available in all build modes) ───────────────────────
/// Nanoseconds since an unspecified epoch (CLOCK_MONOTONIC_RAW).
std::uint64_t clock_ns();
/// Convenience: seconds as double.
double clock_s();

} // namespace psynder::platform::linux_platform

#endif // PSYNDER_PLATFORM_LINUX
