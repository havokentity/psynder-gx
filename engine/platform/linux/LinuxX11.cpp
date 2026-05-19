// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/linux/LinuxX11.cpp
//
// Lane 24 — X11 fallback window creation + vkCreateXcbSurfaceKHR.
//
// Activated when WAYLAND_DISPLAY is not set or the Wayland path fails.
// Uses XCB (preferred over Xlib for thread-safety — DESIGN §11.2).
//
// NEEDS PC VALIDATION: Cannot be tested on the macOS orchestrator.
// Requires libxcb-dev + libxcb-icccm4-dev (Ubuntu 24.04) and an X11 server.

#if PSYNDER_PLATFORM_LINUX
#ifndef PSYNDER_GX_DEDICATED_SERVER

#include "LinuxPlatform.h"

#include <vulkan/vulkan_xcb.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cassert>

// XCB ICCCM for WM_PROTOCOLS / WM_DELETE_WINDOW atoms.
// Ubuntu 24.04: apt install libxcb-icccm4-dev
// If the icccm header is absent, fall back to xcb_intern_atom manually.
#if __has_include(<xcb/xcb_icccm.h>)
#  include <xcb/xcb_icccm.h>
#  define PSYNDER_HAVE_XCB_ICCCM 1
#endif

namespace psynder::platform::linux_platform {

// ─── Atom helpers ────────────────────────────────────────────────────────

static xcb_atom_t intern_atom(xcb_connection_t* conn, const char* name, bool only_if_exists)
{
    xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(conn, only_if_exists ? 1 : 0,
                        static_cast<std::uint16_t>(std::strlen(name)), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    xcb_atom_t atom = XCB_ATOM_NONE;
    if (reply) {
        atom = reply->atom;
        std::free(reply);
    }
    return atom;
}

// ─── Public: create_xcb_window ───────────────────────────────────────────

XcbWindowHandle* create_xcb_window(const WindowCreateInfo& info)
{
    // Connect to the X server.
    int screen_num = 0;
    xcb_connection_t* connection = xcb_connect(nullptr, &screen_num);
    if (xcb_connection_has_error(connection)) {
        std::fprintf(stderr, "[platform-linux] xcb_connect failed — DISPLAY set?\n");
        xcb_disconnect(connection);
        return nullptr;
    }

    // Find the screen.
    const xcb_setup_t*    setup  = xcb_get_setup(connection);
    xcb_screen_iterator_t iter   = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; ++i) {
        xcb_screen_next(&iter);
    }
    xcb_screen_t* screen = iter.data;
    if (!screen) {
        std::fprintf(stderr, "[platform-linux] xcb: no screen found\n");
        xcb_disconnect(connection);
        return nullptr;
    }

    // Create the window.
    xcb_window_t window = xcb_generate_id(connection);
    std::uint32_t event_mask =
        XCB_EVENT_MASK_EXPOSURE       |
        XCB_EVENT_MASK_KEY_PRESS      |
        XCB_EVENT_MASK_KEY_RELEASE    |
        XCB_EVENT_MASK_BUTTON_PRESS   |
        XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY; // ConfigureNotify for resize

    std::uint32_t values[2] = {
        screen->black_pixel,
        event_mask
    };

    xcb_create_window(
        connection,
        XCB_COPY_FROM_PARENT,                    // depth
        window,
        screen->root,                            // parent
        0, 0,                                    // x, y
        static_cast<std::uint16_t>(info.width),
        static_cast<std::uint16_t>(info.height),
        0,                                       // border_width
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
        values);

    // Set title (WM_NAME).
    if (info.title) {
        xcb_change_property(
            connection, XCB_PROP_MODE_REPLACE, window,
            XCB_ATOM_WM_NAME, XCB_ATOM_STRING,
            8, static_cast<std::uint32_t>(std::strlen(info.title)),
            info.title);
    }

    // Wire up WM_DELETE_WINDOW so close button works.
    xcb_atom_t wm_protocols     = intern_atom(connection, "WM_PROTOCOLS",     true);
    xcb_atom_t wm_delete_window = intern_atom(connection, "WM_DELETE_WINDOW", false);

    if (wm_protocols != XCB_ATOM_NONE && wm_delete_window != XCB_ATOM_NONE) {
        xcb_change_property(
            connection, XCB_PROP_MODE_REPLACE, window,
            wm_protocols, XCB_ATOM_ATOM,
            32, 1, &wm_delete_window);
    }

    // Map (show) the window.
    xcb_map_window(connection, window);
    xcb_flush(connection);

    auto* wnd               = new XcbWindowHandle{};
    wnd->connection         = connection;
    wnd->window             = window;
    wnd->wm_delete_window   = wm_delete_window;
    wnd->width              = info.width;
    wnd->height             = info.height;
    wnd->should_close       = false;

    // NEEDS PC VALIDATION: verify window appears and title is set under
    // GNOME + Xwayland, KDE + Xwayland, and a bare X11 session.
    return wnd;
}

// ─── Public: destroy_xcb_window ──────────────────────────────────────────

void destroy_xcb_window(XcbWindowHandle* wnd)
{
    if (!wnd) { return; }
    if (wnd->connection) {
        if (wnd->window) {
            xcb_destroy_window(wnd->connection, wnd->window);
        }
        xcb_disconnect(wnd->connection);
    }
    delete wnd;
}

// ─── Public: poll_xcb_events ─────────────────────────────────────────────

void poll_xcb_events(XcbWindowHandle* wnd)
{
    if (!wnd || !wnd->connection) { return; }

    xcb_generic_event_t* event;
    while ((event = xcb_poll_for_event(wnd->connection)) != nullptr) {
        const std::uint8_t type = event->response_type & 0x7Fu;

        switch (type) {
        case XCB_CLIENT_MESSAGE: {
            auto* cm = reinterpret_cast<xcb_client_message_event_t*>(event);
            if (cm->data.data32[0] == wnd->wm_delete_window) {
                wnd->should_close = true;
            }
            break;
        }
        case XCB_CONFIGURE_NOTIFY: {
            auto* cn = reinterpret_cast<xcb_configure_notify_event_t*>(event);
            wnd->width  = cn->width;
            wnd->height = cn->height;
            break;
        }
        default:
            break;
        }

        std::free(event);
    }
}

// ─── Public: create_vulkan_surface_xcb ───────────────────────────────────

VkSurfaceKHR create_vulkan_surface_xcb(
    xcb_connection_t* connection,
    xcb_window_t      window,
    VkInstance        instance,
    VkResult*         out_result)
{
    assert(connection && "xcb_connection_t must be non-null");
    assert(instance   && "VkInstance must be non-null");

    VkXcbSurfaceCreateInfoKHR ci{};
    ci.sType      = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    ci.connection = connection;
    ci.window     = window;

    auto pfn_create = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR"));

    if (!pfn_create) {
        std::fprintf(stderr,
            "[platform-linux] vkCreateXcbSurfaceKHR not found — "
            "VK_KHR_xcb_surface extension missing?\n");
        if (out_result) { *out_result = VK_ERROR_EXTENSION_NOT_PRESENT; }
        return VK_NULL_HANDLE;
    }

    VkSurfaceKHR out_surface = VK_NULL_HANDLE;
    VkResult     result      = pfn_create(instance, &ci, nullptr, &out_surface);

    if (out_result) { *out_result = result; }
    if (result != VK_SUCCESS) {
        std::fprintf(stderr,
            "[platform-linux] vkCreateXcbSurfaceKHR failed: %d\n",
            static_cast<int>(result));
        return VK_NULL_HANDLE;
    }
    return out_surface;
}

} // namespace psynder::platform::linux_platform

#endif // !PSYNDER_GX_DEDICATED_SERVER
#endif // PSYNDER_PLATFORM_LINUX
