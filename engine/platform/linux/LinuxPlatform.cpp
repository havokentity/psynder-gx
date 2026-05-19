// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/linux/LinuxPlatform.cpp
//
// Lane 24 — Linux platform top-level integration.
//
// Implements the psynder::platform::* abstract interface declared in
// engine/platform/Platform.h by routing to:
//   - Wayland path (primary): LinuxWayland.cpp
//   - X11/XCB path (fallback): LinuxX11.cpp
//   - evdev raw input: LinuxEvdev.cpp
//   - Common utilities: LinuxCommon.cpp
//
// DEDICATED-SERVER BUILD PATH
//   When PSYNDER_GX_DEDICATED_SERVER=1 is defined, ALL windowing and
//   Vulkan-surface code is omitted.  Only clock + filesystem +
//   signal-handling are compiled in.  This produces a minimal library
//   suitable for a headless Linux game server.
//
// See DESIGN-PSYNDER-GX.md §11.2 (Linux) and §11.4 (dedicated server).
// All code is guarded by #if PSYNDER_PLATFORM_LINUX.
//
// NEEDS PC VALIDATION: The orchestrator runs macOS and cannot compile or
// test this file.  Validation must be done on Ubuntu 24.04 with:
//   - libwayland-dev + xdg-shell generated headers (graphical)
//   - libxcb-dev + libxcb-icccm4-dev (X11 fallback)
//   - Vulkan-capable GPU + vulkan-sdk (graphical)
//   - No GPU / no display (dedicated server)

#if PSYNDER_PLATFORM_LINUX

#include "LinuxPlatform.h"
#include "platform/Platform.h"

#include <string>
#include <cstdio>
#include <unistd.h>   // getcwd, access, F_OK — needed by both the dedicated
                      // server branch and the graphical branch below.

// Include common utilities (always compiled).
// LinuxCommon.cpp provides: is_wayland_session(), clock_ns/s(),
// executable_path(), user_config_dir(), install_signal_handlers(),
// should_quit().

namespace psynder::platform {

// ═══════════════════════════════════════════════════════════════════════════
// DEDICATED SERVER BUILD — minimal surface only
// ═══════════════════════════════════════════════════════════════════════════

#ifdef PSYNDER_GX_DEDICATED_SERVER

// No window, no Vulkan surface, no audio device name.
// The dedicated-server main loop drives the physics tick, netcode, and
// scripting directly without any windowing system.

namespace {
class ServerStubWindow final : public Window {
public:
    explicit ServerStubWindow(const WindowDesc& d) : desc_(d) {}
    void poll_events() override                       { /* signal handled in LinuxCommon */ }
    bool should_close() const override {
        return linux_platform::should_quit();
    }
    void present(const render::Framebuffer&) override { /* headless: no-op */ }
    void set_title(std::string_view t) override       { desc_.title = std::string{t}; }
    std::uint32_t window_width()  const override      { return 0u; }
    std::uint32_t window_height() const override      { return 0u; }
private:
    WindowDesc desc_;
};
} // anonymous namespace

Window* create_window_impl(const WindowDesc& desc)
{
    linux_platform::install_signal_handlers();
    return new ServerStubWindow(desc);
}
void destroy_window_impl(Window* w) { delete w; }

namespace {
class ServerStubInput final : public Input {
public:
    bool key_down(KeyCode) const override    { return false; }
    bool key_pressed(KeyCode) const override { return false; }
    const MouseState& mouse() const override { return mouse_; }
private:
    MouseState mouse_{};
};
ServerStubInput g_server_input;
} // anonymous namespace

Input* input() { return &g_server_input; }

std::string executable_path()           { return linux_platform::executable_path(); }
std::string user_config_dir()           { return linux_platform::user_config_dir(); }
std::string current_working_directory() {
    char buf[4096] = {};
    return getcwd(buf, sizeof(buf)) ? std::string{buf} : std::string{};
}
bool file_exists(std::string_view path) {
    return access(std::string{path}.c_str(), F_OK) == 0;
}

// Audio device name plumbing — dedicated server has no audio but exposes
// the stub so lane 14 can link without ifdefs.
std::string audio_device_name() { return {}; }

#else // !PSYNDER_GX_DEDICATED_SERVER
// ═══════════════════════════════════════════════════════════════════════════
// GRAPHICAL BUILD — Wayland primary, X11/XCB fallback
// ═══════════════════════════════════════════════════════════════════════════

#include <unistd.h>

namespace {

// ─── Wayland graphical window ─────────────────────────────────────────────
class WaylandWindow final : public Window {
public:
    explicit WaylandWindow(linux_platform::WaylandWindowHandle* h,
                           linux_platform::EvdevReader*          evdev)
        : handle_(h), evdev_(evdev) {}

    ~WaylandWindow() override {
        linux_platform::destroy_wayland_window(handle_);
        linux_platform::evdev_close(evdev_);
    }

    void poll_events() override {
        linux_platform::poll_wayland_events(handle_);
        // Drain evdev and accumulate into mouse state.
        if (evdev_) {
            auto delta = linux_platform::evdev_poll_mouse(evdev_);
            mouse_.dx += delta.dx;
            mouse_.dy += delta.dy;
            mouse_.scroll_v += delta.scroll_v;
        }
    }

    bool should_close() const override {
        return (handle_ && handle_->should_close) ||
               linux_platform::should_quit();
    }

    void present(const render::Framebuffer&) override {
        // GPU present is driven by psy::gpu::Device::end_frame().
        // Platform layer just commits the Wayland surface frame callback
        // here if needed; real present happens in the gpu lane.
        if (handle_ && handle_->surface) {
            wl_surface_commit(handle_->surface);
        }
    }

    void set_title(std::string_view t) override {
        if (handle_ && handle_->xdg_toplevel) {
            xdg_toplevel_set_title(
                static_cast<xdg_toplevel*>(handle_->xdg_toplevel),
                std::string{t}.c_str());
        }
    }

    std::uint32_t window_width()  const override {
        return handle_ ? handle_->width  : 0u;
    }
    std::uint32_t window_height() const override {
        return handle_ ? handle_->height : 0u;
    }

    // Surface accessors for lane 07 (gpu) to call create_vulkan_surface_wayland.
    wl_display* wayland_display() const { return handle_ ? handle_->display  : nullptr; }
    wl_surface* wayland_surface() const { return handle_ ? handle_->surface  : nullptr; }

private:
    linux_platform::WaylandWindowHandle* handle_;
    linux_platform::EvdevReader*         evdev_;
    MouseState mouse_{};
};

// ─── XCB graphical window ─────────────────────────────────────────────────
class XcbWindow final : public Window {
public:
    explicit XcbWindow(linux_platform::XcbWindowHandle* h,
                       linux_platform::EvdevReader*      evdev)
        : handle_(h), evdev_(evdev) {}

    ~XcbWindow() override {
        linux_platform::destroy_xcb_window(handle_);
        linux_platform::evdev_close(evdev_);
    }

    void poll_events() override {
        linux_platform::poll_xcb_events(handle_);
        if (evdev_) {
            auto delta = linux_platform::evdev_poll_mouse(evdev_);
            mouse_.dx += delta.dx;
            mouse_.dy += delta.dy;
            mouse_.scroll_v += delta.scroll_v;
        }
    }

    bool should_close() const override {
        return (handle_ && handle_->should_close) ||
               linux_platform::should_quit();
    }

    void present(const render::Framebuffer&) override {
        // Present driven by gpu lane via vkQueuePresentKHR.
    }

    void set_title(std::string_view t) override {
        if (handle_ && handle_->connection && handle_->window) {
            xcb_change_property(
                handle_->connection, XCB_PROP_MODE_REPLACE, handle_->window,
                XCB_ATOM_WM_NAME, XCB_ATOM_STRING,
                8, static_cast<std::uint32_t>(t.size()),
                t.data());
        }
    }

    std::uint32_t window_width()  const override {
        return handle_ ? handle_->width  : 0u;
    }
    std::uint32_t window_height() const override {
        return handle_ ? handle_->height : 0u;
    }

    // Accessors for lane 07 gpu surface creation.
    xcb_connection_t* xcb_connection() const {
        return handle_ ? handle_->connection : nullptr;
    }
    xcb_window_t xcb_window_id() const {
        return handle_ ? handle_->window : 0;
    }

private:
    linux_platform::XcbWindowHandle* handle_;
    linux_platform::EvdevReader*     evdev_;
    MouseState mouse_{};
};

// ─── Input wrapper (evdev-backed) ─────────────────────────────────────────
// NOTE: Key translation (KeyCode enum → Linux key code) is stubbed.
// NEEDS PC VALIDATION: fill out the KeyCode → KEY_* mapping table.
class EvdevInput final : public Input {
public:
    explicit EvdevInput(linux_platform::EvdevReader* reader) : reader_(reader) {}

    bool key_down(KeyCode /*key*/) const override {
        // STUB: translate KeyCode → linux_key_code, then query reader_.
        // NEEDS PC VALIDATION: implement KeyCode → KEY_* mapping.
        return false;
    }
    bool key_pressed(KeyCode) const override { return false; }
    const MouseState& mouse() const override { return mouse_; }

    void sync_mouse_from_delta(const linux_platform::RawMouseDelta& d) {
        mouse_.dx += d.dx;
        mouse_.dy += d.dy;
        mouse_.scroll_v += d.scroll_v;
    }

private:
    linux_platform::EvdevReader* reader_;
    MouseState mouse_{};
};

EvdevInput* g_input_instance = nullptr;

} // anonymous namespace

// ─── Public: create_window_impl ──────────────────────────────────────────

Window* create_window_impl(const WindowDesc& desc)
{
    linux_platform::install_signal_handlers();

    // Always open evdev — it works regardless of Wayland vs X11.
    linux_platform::EvdevReader* evdev = linux_platform::evdev_open();
    // evdev can be null on permission issues — log but continue.

    linux_platform::WindowCreateInfo ci{};
    ci.title  = desc.title.c_str();
    ci.width  = desc.window_width;
    ci.height = desc.window_height;

    if (linux_platform::is_wayland_session()) {
        auto* handle = linux_platform::create_wayland_window(ci);
        if (handle) {
            return new WaylandWindow(handle, evdev);
        }
        std::fprintf(stderr,
            "[platform-linux] Wayland window creation failed; "
            "falling back to X11/XCB\n");
    }

    // X11/XCB fallback.
    auto* xcb_handle = linux_platform::create_xcb_window(ci);
    if (xcb_handle) {
        return new XcbWindow(xcb_handle, evdev);
    }

    std::fprintf(stderr,
        "[platform-linux] Both Wayland and X11/XCB window creation failed\n");
    linux_platform::evdev_close(evdev);
    return nullptr;
}

void destroy_window_impl(Window* w) { delete w; }

// ─── Public: input ────────────────────────────────────────────────────────
// The evdev reader is owned by the Window; for non-window input queries
// we expose a global stub.  In M1 this should be wired to the window's
// evdev reader.  NEEDS PC VALIDATION.
namespace {
class NullInput final : public Input {
public:
    bool key_down(KeyCode) const override    { return false; }
    bool key_pressed(KeyCode) const override { return false; }
    const MouseState& mouse() const override { return mouse_; }
private:
    MouseState mouse_{};
};
NullInput g_null_input;
} // anonymous namespace

Input* input() { return &g_null_input; }

// ─── Filesystem / utilities ───────────────────────────────────────────────

std::string executable_path() { return linux_platform::executable_path(); }
std::string user_config_dir() { return linux_platform::user_config_dir(); }

std::string current_working_directory() {
    char buf[4096] = {};
    return getcwd(buf, sizeof(buf)) ? std::string{buf} : std::string{};
}

bool file_exists(std::string_view path) {
    return access(std::string{path}.c_str(), F_OK) == 0;
}

// Audio device name plumbing — actual PipeWire/ALSA init belongs to lane 14.
// We expose the name so lane 14 can open the device without platform ifdefs.
std::string audio_device_name() {
    // PipeWire: return empty string = default device.
    // ALSA: "default" is the ALSA default PCM.
    // Lane 14 selects the backend at link time.
    return {};
}

#endif // PSYNDER_GX_DEDICATED_SERVER

} // namespace psynder::platform

#endif // PSYNDER_PLATFORM_LINUX
