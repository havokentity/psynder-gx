// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/macos/MacPlatform.cpp
//
// Lane 25 — legacy psynder::platform::{Window,Input} shim, retained so
// the orchestrator-owned platform aggregator (engine/platform/Platform.cpp)
// continues to link. The Wave-0 stub merely returned a window that exited
// after one frame; this revision delegates to the real GX surface in
// MacosPlatform.mm (NSWindow + CAMetalLayer + IOKit raw mouse).
//
// The legacy Window::present(const render::Framebuffer&) is a CPU-raster
// artifact carried over from sister Psynder. Psynder-GX does NOT present
// via the CPU; lane 07 (gpu) drives CAMetalLayer end-to-end. We keep the
// override as a no-op so the build stays clean.
//
// NOTE: This TU intentionally avoids depending on engine/core/* (Lane 01
// has not landed yet at the time of writing). It uses the standard C++
// types directly and bridges to the shared psynder::platform interfaces.

#if PSYNDER_PLATFORM_MACOS

#include "platform/Platform.h"
#include "platform/macos/PublicPlatformMacos.h"
#include "platform/macos/MacosPlatform_internal.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace psynder::platform {

namespace {

class MacWindow final : public Window {
public:
    explicit MacWindow(const WindowDesc& d) : desc_(d) {
        macos::WindowDesc md{};
        md.title         = desc_.title.c_str();
        md.window_width  = desc_.window_width;
        md.window_height = desc_.window_height;
        md.resizable     = desc_.resizable;
        md.high_dpi      = true;
        win_ = macos::create_window(md);
    }
    ~MacWindow() override {
        if (win_) macos::destroy_window(win_);
        win_ = nullptr;
    }

    void poll_events() override            { macos::pump_events(); }
    bool should_close() const override     { return win_ ? macos::should_close(win_) : true; }
    void present(const render::Framebuffer&) override {
        // CPU-framebuffer present is a sister-Psynder concept; GX lane 07
        // owns the present path via psy::gpu::end_frame(). Intentional no-op.
    }
    void set_title(std::string_view t) override { desc_.title = std::string{t}; /* TODO: forward to NSWindow */ }
    u32  window_width()  const override    { return win_ ? macos::point_width(win_)  : desc_.window_width;  }
    u32  window_height() const override    { return win_ ? macos::point_height(win_) : desc_.window_height; }

    // Exposed to lane 07 via DeviceDesc::native_window_handle. The
    // platform aggregator does not surface this in its public API yet;
    // callers that need it can downcast or call macos::create_window
    // directly and pass macos::native_layer(win) to DeviceDesc.
    macos::Window* gx_native() const { return win_; }

private:
    macos::Window* win_ = nullptr;
    WindowDesc     desc_;
};

class MacInput final : public Input {
public:
    bool key_down(KeyCode k) const override {
        const auto idx = static_cast<std::size_t>(k);
        const auto& ks = macos::keyboard_state();
        if (idx == 0 || idx >= macos::kKeyCount) return false;
        return ks.down[idx];
    }
    bool key_pressed(KeyCode k) const override {
        const auto idx = static_cast<std::size_t>(k);
        const auto& ks = macos::keyboard_state();
        if (idx == 0 || idx >= macos::kKeyCount) return false;
        return ks.pressed[idx];
    }
    const MouseState& mouse() const override {
        // Fold IOKit raw mouse into the legacy MouseState. Each call to
        // mouse_delta_raw() snapshots-and-resets, so we accumulate into
        // ms_ to keep deltas stable for callers that poll once per frame
        // through this legacy interface.
        auto d = macos::mouse_delta_raw();
        ms_.dx += static_cast<f32>(d.dx);
        ms_.dy += static_cast<f32>(d.dy);
        ms_.wheel += static_cast<f32>(d.wheel);
        ms_.left   = d.left_down;
        ms_.right  = d.right_down;
        ms_.middle = d.middle_down;
        return ms_;
    }

private:
    mutable MouseState ms_{};
};

MacInput g_input;

} // namespace

Window* create_window_impl(const WindowDesc& desc) { return new MacWindow(desc); }
void    destroy_window_impl(Window* w)             { delete w; }

Input* input() { return &g_input; }

std::string executable_path()             { return std::string{macos::executable_path()}; }
std::string user_config_dir()             { return std::string{macos::user_config_dir()}; }
std::string current_working_directory()   { return std::string{macos::current_working_directory()}; }
bool        file_exists(std::string_view p) {
    std::string s{p};
    return macos::file_exists(s.c_str());
}

} // namespace psynder::platform

#endif // PSYNDER_PLATFORM_MACOS
