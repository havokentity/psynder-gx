// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/macos/PublicPlatformMacos.h
//
// Lane 25 — macOS Apple Silicon platform PUBLIC CONTRACT (Wave B).
//
// Surface-creation glue between AppKit (NSWindow) and lane 07's
// psy::gpu::Device on the native Metal backend. Per DESIGN §11.3, GX uses
// native Metal — NOT MoltenVK. The NSWindow's contentView is layer-backed
// by a CAMetalLayer; lane 07 reads that layer via DeviceDesc::native_
// window_handle and drives it end-to-end (pixelFormat, nextDrawable, etc).
//
// This header is INTENTIONALLY free of #import <AppKit/...> so other lanes
// can include it from plain .cpp translation units. The AppKit / Metal /
// IOKit / GameController / CoreAudio interop is hidden behind opaque
// void* handles and lives in the .mm sources under this directory.
//
// Threading: every entry point is callable from the main thread only.
// Lane 07's begin_frame / end_frame is called from the main thread as
// well — there is no second AppKit thread in GX.

#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __OBJC__
@class NSWindow;
@class CAMetalLayer;
#endif

namespace psynder::platform::macos {

// ─── Opaque handles (private impl in .mm) ───────────────────────────────
struct Window;   // owns the NSWindow + CAMetalLayer + delegate + view

// ─── Window creation ────────────────────────────────────────────────────
struct WindowDesc {
    const char*   title          = "Psynder-GX";
    std::uint32_t window_width   = 1280;
    std::uint32_t window_height  = 720;
    bool          resizable      = true;
    bool          high_dpi       = true;   // attach contentsScale = backingScaleFactor
};

// Open an NSWindow + attach a CAMetalLayer-backed contentView. The window
// is centred + ordered front + activated. Returns nullptr on failure (no
// Metal device, AppKit not bootstrap-able from this process, etc).
Window* create_window(const WindowDesc&);
void    destroy_window(Window*);

// Accessors. Returned pointers are valid for the Window's lifetime.
//   native_window()  → NSWindow*    (typed as void* for non-ObjC callers)
//   native_layer()   → CAMetalLayer* (typed as void* for non-ObjC callers)
//   native_view()    → NSView*      (typed as void* for non-ObjC callers)
//
// Lane 07's create_device() reads native_layer() via DeviceDesc::
// native_window_handle and binds it to MTLDevice / drawableSize.
void* native_window(Window*);
void* native_layer (Window*);
void* native_view  (Window*);

// Convenience for callers that opened their OWN NSWindow elsewhere (e.g.
// the editor host) and want lane 25 to attach a CAMetalLayer to its
// contentView. Returns the CAMetalLayer* on success, nullptr on failure
// (e.g. the window has no contentView). Idempotent — re-attaching to a
// window that already has a CAMetalLayer-backed view returns the existing
// layer.
//
// ns_window must be an NSWindow*; typed as void* so this header stays
// safe to include from plain C++ TUs.
void* create_metal_layer(void* ns_window);

// Current backing-store dimensions in pixels (i.e. drawableSize). Reflects
// any user resize since the last call. Lane 07 calls these to size its
// swapchain / depth attachments.
std::uint32_t drawable_width (Window*);
std::uint32_t drawable_height(Window*);

// Logical point size (NSView bounds, unscaled). Useful for HiDPI math.
std::uint32_t point_width (Window*);
std::uint32_t point_height(Window*);

// True once the user clicked the close box / hit Cmd-Q / pressed Esc.
bool should_close(Window*);

// Convenience: explicit close from code (e.g. demo time-limit hit).
void request_close(Window*);

// ─── Main-thread event pump ─────────────────────────────────────────────
// Pumps every queued NSEvent then returns. Call once per frame from the
// main thread before invoking lane 07's begin_frame. Resets the per-frame
// raw-mouse delta accumulator (so the value returned by mouse_delta_raw
// always covers the interval between consecutive pump_events calls).
// Also clears the per-frame edge-triggered key-pressed state and records
// new keyboard transitions into the keyboard state (see key_down/key_pressed).
void pump_events();

// UTF-8 text typed during the most recent pump_events() call. This is for
// console/text-entry surfaces; gameplay should use platform::Input key states.
// Copies at most capacity-1 bytes and nul-terminates when capacity > 0.
std::size_t text_input_utf8(char* dst, std::size_t capacity);

struct ConsoleShortcuts {
    bool copy = false;
    bool cut = false;
    bool paste = false;
    bool select_all = false;
    const char* paste_text = "";
};
ConsoleShortcuts consume_console_shortcuts();
void set_clipboard_text(const char* text);

// Register a window to receive Esc-key close signals. When Escape is pressed
// during pump_events, request_close(w) is called on the registered window.
// run_loop() sets this automatically; call it directly when driving the event
// loop manually. Pass nullptr to disarm. Returns the previously registered
// window (for save/restore in nested run loops).
Window* set_esc_close_target(Window* w);

// Convenience wrapper: pump_events + invoke `frame()` until the window
// reports should_close. Most samples call this. `frame_user` is forwarded
// verbatim to the callback. Returns the number of frames pumped.
using FrameCallback = void (*)(void* user, Window* window, double dt_seconds);
std::uint64_t run_loop(Window* window, FrameCallback frame, void* frame_user);

// ─── Raw mouse (IOKit HID — sub-frame poll for FPS aim) ─────────────────
// The IOKit HID Manager is armed lazily on the first call. Returns the
// accumulated delta in display-coordinate pixels since the last
// pump_events() reset. Sub-frame polling: call this multiple times per
// frame from the input thread to get a moving aim integral.
struct MouseDelta {
    double dx          = 0.0;
    double dy          = 0.0;
    double wheel       = 0.0;
    double x           = 0.0;
    double y           = 0.0;
    bool   left_down   = false;
    bool   right_down  = false;
    bool   middle_down = false;
};
MouseDelta mouse_delta_raw();

// ─── Gamepad (GameController.framework) ─────────────────────────────────
// Idempotent. Arms connect / disconnect notifications + seeds the count
// with controllers already paired. Safe to call multiple times.
void gamepad_arm();
std::uint32_t gamepad_count();

// ─── CoreAudio device plumbing for lane 14 ──────────────────────────────
// Returns the system default-output device's user-friendly name (UTF-8).
// Lane 14 owns the mixer + AUHAL setup; this is just the lookup.
// Buffer is internal to the lane; pointer is valid until the next call.
const char* default_audio_device_name();

// Default output sample rate (Hz) reported by CoreAudio. Lane 14 uses
// this as the device-format hint when opening its AUHAL.
std::uint32_t default_audio_sample_rate();

// ─── Process / FS helpers (kept in plain C++ TU; no AppKit) ─────────────
// Provided here so editor / asset lanes can locate user-data + exe path
// without dragging AppKit into their builds.
const char* executable_path();          // absolute path, resolved via realpath
const char* user_config_dir();          // ~/Library/Application Support/Psynder-GX
const char* current_working_directory();
bool        file_exists(const char* path);

} // namespace psynder::platform::macos
