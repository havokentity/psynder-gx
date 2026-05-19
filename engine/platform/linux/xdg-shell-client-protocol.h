// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/linux/xdg-shell-client-protocol.h
//
// STUB / PLACEHOLDER — Lane 24.
//
// This file is a compile-time placeholder for the real xdg-shell generated
// header produced by wayland-scanner from the xdg-shell.xml protocol
// description (part of wayland-protocols).
//
// HOW TO GENERATE THE REAL HEADER (Ubuntu 24.04):
//
//   apt install wayland-protocols
//   PROTO_DIR=$(pkg-config --variable=pkgdatadir wayland-protocols)
//   wayland-scanner client-header \
//       $PROTO_DIR/stable/xdg-shell/xdg-shell.xml \
//       xdg-shell-client-protocol.h
//   wayland-scanner private-code \
//       $PROTO_DIR/stable/xdg-shell/xdg-shell.xml \
//       xdg-shell-protocol.c
//
// The generated header + C source should be placed in:
//   ${CMAKE_BINARY_DIR}/generated/xdg-shell/
// and the CMakeLists.txt adds that directory to include paths.
//
// The root CMakeLists.txt (orchestrator-owned) should add a custom_command
// to run wayland-scanner at configure time.
//
// NEEDS PC VALIDATION: Replace this stub with the real generated header
// before the graphical Linux build can link.
//
// ─────────────────────────────────────────────────────────────────────────
// Minimal stub declarations so static-analysis / CI on macOS doesn't error
// on undeclared symbols referenced in LinuxWayland.cpp.
// These types/functions are normally provided by the generated header.
// ─────────────────────────────────────────────────────────────────────────

#pragma once

#if PSYNDER_PLATFORM_LINUX && !defined(PSYNDER_GX_DEDICATED_SERVER)

#include <wayland-client.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── xdg_wm_base ──────────────────────────────────────────────────────────
struct xdg_wm_base;

extern const struct wl_interface xdg_wm_base_interface;

struct xdg_wm_base_listener {
    void (*ping)(void* data, struct xdg_wm_base* shell, uint32_t serial);
};

void xdg_wm_base_add_listener(
    struct xdg_wm_base* shell,
    const struct xdg_wm_base_listener* listener,
    void* data);
void xdg_wm_base_pong(struct xdg_wm_base* shell, uint32_t serial);
void xdg_wm_base_destroy(struct xdg_wm_base* shell);

// ── xdg_surface ──────────────────────────────────────────────────────────
struct xdg_surface;

struct xdg_surface_listener {
    void (*configure)(void* data, struct xdg_surface* surface, uint32_t serial);
};

struct xdg_surface* xdg_wm_base_get_xdg_surface(
    struct xdg_wm_base* shell, struct wl_surface* surface);
void xdg_surface_add_listener(
    struct xdg_surface* surface,
    const struct xdg_surface_listener* listener,
    void* data);
void xdg_surface_ack_configure(struct xdg_surface* surface, uint32_t serial);
void xdg_surface_destroy(struct xdg_surface* surface);

// ── xdg_toplevel ─────────────────────────────────────────────────────────
struct xdg_toplevel;

// xdg_toplevel::configure_bounds was added in xdg-shell protocol v4.
#define XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION 4

struct xdg_toplevel_listener {
    void (*configure)(void* data, struct xdg_toplevel* toplevel,
                      int32_t width, int32_t height, struct wl_array* states);
    void (*close)(void* data, struct xdg_toplevel* toplevel);
    void (*configure_bounds)(void* data, struct xdg_toplevel* toplevel,
                             int32_t width, int32_t height);
};

struct xdg_toplevel* xdg_surface_get_toplevel(struct xdg_surface* surface);
void xdg_toplevel_add_listener(
    struct xdg_toplevel* toplevel,
    const struct xdg_toplevel_listener* listener,
    void* data);
void xdg_toplevel_set_title(struct xdg_toplevel* toplevel, const char* title);
void xdg_toplevel_set_app_id(struct xdg_toplevel* toplevel, const char* app_id);
void xdg_toplevel_destroy(struct xdg_toplevel* toplevel);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // PSYNDER_PLATFORM_LINUX && !PSYNDER_GX_DEDICATED_SERVER
