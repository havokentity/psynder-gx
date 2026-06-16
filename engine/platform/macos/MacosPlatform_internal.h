// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/macos/MacosPlatform_internal.h
//
// Lane 25 — internal shared declarations across this lane's translation
// units. Not part of the public surface (see PublicPlatformMacos.h for
// the consumer-facing API). Kept .h so the Objective-C++ AppKit/Metal
// driver and the plain C++ filesystem TU share the lane-local helpers
// without forcing AppKit on the latter.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "platform/macos/PublicPlatformMacos.h"
#include "platform/Platform.h"  // KeyCode, kKeyCodeCount

namespace psynder::platform::macos {

// ─── Raw mouse state (IOKit HID Manager) ────────────────────────────────
// Lives here so the .mm bridge can publish deltas + the same struct can
// be read from the public mouse_delta_raw() accessor.
struct RawMouseState {
    // Accumulated relative motion since the last pump_events() reset.
    // IOKit reports HID relative deltas in pointer units (typically pixels
    // for mice with low DPI; high-DPI mice report integer ticks at higher
    // resolution). Stored as double so accumulation across many sub-frame
    // samples does not lose precision.
    std::atomic<double> accum_dx{0.0};
    std::atomic<double> accum_dy{0.0};
    std::atomic<double> accum_wheel{0.0};
    std::atomic<double> x{0.0};
    std::atomic<double> y{0.0};

    // Pressed state is sticky between pumps (button-down across frames).
    std::atomic<bool> left_down{false};
    std::atomic<bool> right_down{false};
    std::atomic<bool> middle_down{false};
};

RawMouseState& raw_mouse_state();

// Arm the IOKit HID Manager for relative-mouse events. Idempotent; safe to
// call from create_window. Returns true if the manager opened.
bool iokit_mouse_arm();

// Pump-events reset: zeros the accumulated deltas. Returns the values held
// at the moment of reset so the public mouse_delta_raw() returns a
// consistent snapshot for the frame that just ended.
struct RawMouseSnapshot {
    double dx          = 0.0;
    double dy          = 0.0;
    double wheel       = 0.0;
    double x           = 0.0;
    double y           = 0.0;
    bool   left_down   = false;
    bool   right_down  = false;
    bool   middle_down = false;
};
RawMouseSnapshot raw_mouse_snapshot_and_reset();

// ─── Keyboard state ──────────────────────────────────────────────────────
// Indexed by psynder::platform::KeyCode cast to std::size_t.
// key_down[k]    = currently held (level trigger)
// key_pressed[k] = transitioned 0→1 this frame (edge trigger, cleared by
//                  keyboard_begin_frame() called from pump_events())
//
// Both arrays are modified only on the main thread (inside the NSEvent
// pump), so plain bool suffices. They are public (not atomic) because
// Input::key_down / key_pressed are also called from the main thread only.

static constexpr std::size_t kKeyCount =
    static_cast<std::size_t>(psynder::platform::KeyCode::Count);

struct KeyboardState {
    bool down   [kKeyCount]{};
    bool pressed[kKeyCount]{};
};

// Process a single NSEvent keycode + direction into the global keyboard
// state. is_down = true for KeyDown, false for KeyUp.
// Called from MacosPlatform.mm's NSEvent loop.
void keyboard_event(psynder::platform::KeyCode key, bool is_down);

// Clear the edge-triggered pressed[] array. Must be called at the START of
// each pump_events() before dispatching new events so pressed[] only ever
// describes keys that transitioned this frame.
void keyboard_begin_frame();

// Read-only access for MacInput::key_down / key_pressed.
const KeyboardState& keyboard_state();

// ─── Filesystem helpers (live in MacosPlatformFs.cpp, plain C++) ────────
std::string fs_executable_path();
std::string fs_user_config_dir();
std::string fs_current_working_directory();
bool        fs_file_exists(std::string_view path);

} // namespace psynder::platform::macos
