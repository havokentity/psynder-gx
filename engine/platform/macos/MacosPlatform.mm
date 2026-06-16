// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/macos/MacosPlatform.mm
//
// Lane 25 — macOS Apple Silicon platform surface for Psynder-GX.
//
// Owns:
//   * NSWindow + a layer-backed NSView whose backing layer is CAMetalLayer.
//   * Main-thread NSEvent pump (non-blocking).
//   * Raw mouse via IOKit HID Manager (sub-frame poll for FPS aim).
//   * Gamepad enumeration via GameController.framework.
//   * Default-output audio device lookup via CoreAudio (HAL).
//
// Does NOT own:
//   * The Metal device, command queue, or any drawables — lane 07 (gpu)
//     reads our CAMetalLayer via DeviceDesc::native_window_handle and
//     drives the present path end-to-end (DESIGN §11.3).
//   * The audio mixer / AUHAL render callback — lane 14 owns that and uses
//     default_audio_device_name() / default_audio_sample_rate() to bind.
//
// Threading: all entry points are main-thread-only. AppKit demands that.
// The IOKit HID callbacks fire on a dedicated CFRunLoop the manager
// schedules; we use atomics to lift their deltas onto the main thread.

#if !__has_feature(objc_arc)
#error "MacosPlatform.mm requires ARC. CMake enables it for .mm via -fobjc-arc."
#endif

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#import <CoreAudio/CoreAudio.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/hid/IOHIDKeys.h>
#import <CoreGraphics/CoreGraphics.h>

#include "MacosPlatform_internal.h"
#include "MacosKeyMap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

// ─── Forward decls (Objective-C classes declared below) ─────────────────
@class PsynderGxWindowDelegate;
@class PsynderGxMetalView;

namespace psynder::platform::macos {

// ─── Per-process raw-mouse state ────────────────────────────────────────
RawMouseState& raw_mouse_state() {
    static RawMouseState s;
    return s;
}

// FPS mouse-capture flag. When true the OS cursor is hidden + locked (see
// set_mouse_captured): the IOKit HID X/Y accumulation is suppressed and the
// relative deltas come from NSEvent.deltaX/deltaY instead, which keep flowing
// while the cursor position is frozen and need no Input Monitoring grant.
// Read from the IOKit callback thread, so it's atomic.
static std::atomic<bool> g_mouse_captured{false};

// ─── Keyboard state (main-thread-only) ──────────────────────────────────
static KeyboardState g_keyboard_state;
static char g_text_input[512];
static std::size_t g_text_input_size = 0;
static ConsoleShortcuts g_console_shortcuts;
static std::string g_console_paste_text;

const KeyboardState& keyboard_state() {
    return g_keyboard_state;
}

void keyboard_begin_frame() {
    // Clear the edge-triggered pressed[] array so only transitions THIS
    // frame come through. Called at the top of pump_events().
    __builtin_memset(g_keyboard_state.pressed, 0, sizeof(g_keyboard_state.pressed));
    g_text_input_size = 0;
    g_console_shortcuts = {};
    g_console_paste_text.clear();
}

void keyboard_event(psynder::platform::KeyCode key, bool is_down) {
    const auto idx = static_cast<std::size_t>(key);
    if (idx == 0 || idx >= kKeyCount) return;  // Unknown or out-of-range
    if (is_down && !g_keyboard_state.down[idx]) {
        // 0→1 transition: set edge-triggered pressed flag.
        g_keyboard_state.pressed[idx] = true;
    }
    g_keyboard_state.down[idx] = is_down;
}

void append_text_input(NSString* text) {
    if (!text) return;
    const char* utf8 = [text UTF8String];
    if (!utf8) return;
    while (*utf8 && g_text_input_size + 1u < sizeof(g_text_input)) {
        const unsigned char c = static_cast<unsigned char>(*utf8++);
        if (c >= 0x20u && c != 0x7Fu) {
            g_text_input[g_text_input_size++] = static_cast<char>(c);
        }
    }
}

void capture_command_shortcut(NSEvent* event) {
    if (event.type != NSEventTypeKeyDown) {
        return;
    }
    NSString* chars = event.charactersIgnoringModifiers;
    if (!chars || chars.length == 0) {
        return;
    }
    const unichar c = [[chars lowercaseString] characterAtIndex:0];
    switch (c) {
        case 'c':
            g_console_shortcuts.copy = true;
            break;
        case 'x':
            g_console_shortcuts.cut = true;
            break;
        case 'a':
            g_console_shortcuts.select_all = true;
            break;
        case 'v': {
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            NSString* s = [pb stringForType:NSPasteboardTypeString];
            g_console_paste_text =
                s ? std::string{[s UTF8String]} : std::string{};
            g_console_shortcuts.paste = true;
            g_console_shortcuts.paste_text = g_console_paste_text.c_str();
            break;
        }
        default:
            break;
    }
}

// ─── NSApp lazy bootstrap ───────────────────────────────────────────────
// AppKit must be initialised on the main thread before any NSWindow is
// constructed. Idempotent so re-entering create_window from samples that
// recreate windows does not re-install the activation policy.
static void ensure_app_initialised() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        // We drive the event loop manually via nextEventMatchingMask each
        // frame, so we never call [NSApp run] (which would block).
    });
}

// ─── IOKit HID Manager (raw mouse) ──────────────────────────────────────
namespace {

struct IOKitState {
    IOHIDManagerRef       manager = nullptr;
    std::atomic<bool>     armed{false};
};

IOKitState& iokit_state() {
    static IOKitState s;
    return s;
}

void iokit_input_value_cb(void* /*context*/, IOReturn /*result*/,
                          void* /*sender*/, IOHIDValueRef value) {
    if (!value) return;
    IOHIDElementRef elem = IOHIDValueGetElement(value);
    if (!elem) return;
    const uint32_t usage_page = IOHIDElementGetUsagePage(elem);
    const uint32_t usage      = IOHIDElementGetUsage(elem);
    const CFIndex  raw        = IOHIDValueGetIntegerValue(value);
    auto& st = raw_mouse_state();

    if (usage_page == kHIDPage_GenericDesktop) {
        // While the cursor is captured, NSEvent relative deltas are the single
        // source of X/Y motion (see pump_events) — skip the IOKit X/Y path so
        // the two don't double-count.
        const bool captured = g_mouse_captured.load(std::memory_order_relaxed);
        switch (usage) {
            case kHIDUsage_GD_X: {
                if (captured) break;
                // Atomic accumulation. fetch_add isn't defined on double in
                // C++20 atomics by default for all stdlib versions; do CAS.
                double cur = st.accum_dx.load(std::memory_order_relaxed);
                while (!st.accum_dx.compare_exchange_weak(
                           cur, cur + static_cast<double>(raw),
                           std::memory_order_relaxed)) { /* retry */ }
                break;
            }
            case kHIDUsage_GD_Y: {
                if (captured) break;
                double cur = st.accum_dy.load(std::memory_order_relaxed);
                while (!st.accum_dy.compare_exchange_weak(
                           cur, cur + static_cast<double>(raw),
                           std::memory_order_relaxed)) { /* retry */ }
                break;
            }
            case kHIDUsage_GD_Wheel: {
                double cur = st.accum_wheel.load(std::memory_order_relaxed);
                while (!st.accum_wheel.compare_exchange_weak(
                           cur, cur + static_cast<double>(raw),
                           std::memory_order_relaxed)) { /* retry */ }
                break;
            }
            default: break;
        }
    } else if (usage_page == kHIDPage_Button) {
        const bool down = (raw != 0);
        switch (usage) {
            case 1: st.left_down.store(down,   std::memory_order_relaxed); break;
            case 2: st.right_down.store(down,  std::memory_order_relaxed); break;
            case 3: st.middle_down.store(down, std::memory_order_relaxed); break;
            default: break;
        }
    }
}

CFDictionaryRef make_match_dict(uint32_t usage_page, uint32_t usage) {
    CFNumberRef pageNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage_page);
    CFNumberRef useNum  = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    const void* keys  [] = { CFSTR(kIOHIDDeviceUsagePageKey), CFSTR(kIOHIDDeviceUsageKey) };
    const void* vals  [] = { pageNum, useNum };
    CFDictionaryRef d = CFDictionaryCreate(
        kCFAllocatorDefault, keys, vals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(pageNum);
    CFRelease(useNum);
    return d;
}

void add_atomic_double(std::atomic<double>& target, double delta) {
    double cur = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(
               cur, cur + delta, std::memory_order_relaxed)) { /* retry */ }
}

} // namespace

bool iokit_mouse_arm() {
    auto& st = iokit_state();
    if (st.armed.load(std::memory_order_acquire)) return true;

    st.manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!st.manager) return false;

    // Match mice + pointer-like devices on the GenericDesktop usage page.
    CFDictionaryRef matches[] = {
        make_match_dict(kHIDPage_GenericDesktop, kHIDUsage_GD_Mouse),
        make_match_dict(kHIDPage_GenericDesktop, kHIDUsage_GD_Pointer),
    };
    CFArrayRef matchArr = CFArrayCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const void**>(matches),
        sizeof(matches) / sizeof(matches[0]),
        &kCFTypeArrayCallBacks);
    IOHIDManagerSetDeviceMatchingMultiple(st.manager, matchArr);
    CFRelease(matchArr);
    for (auto& m : matches) CFRelease(m);

    IOHIDManagerRegisterInputValueCallback(st.manager, iokit_input_value_cb, nullptr);
    IOHIDManagerScheduleWithRunLoop(st.manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOReturn rc = IOHIDManagerOpen(st.manager, kIOHIDOptionsTypeNone);
    if (rc != kIOReturnSuccess) {
        // On macOS 11+ the user must grant Input Monitoring permission for
        // background apps. Foreground (foreground-key) apps get IOKit HID
        // for free. If open fails we still keep the manager around for
        // retry on the next call.
        IOHIDManagerUnscheduleFromRunLoop(st.manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
        return false;
    }
    st.armed.store(true, std::memory_order_release);
    return true;
}

RawMouseSnapshot raw_mouse_snapshot_and_reset() {
    auto& st = raw_mouse_state();
    RawMouseSnapshot out{};
    // exchange returns the prior value atomically and zeroes it for the
    // next accumulation window (the next pump_events).
    out.dx          = st.accum_dx.exchange(0.0, std::memory_order_acq_rel);
    out.dy          = st.accum_dy.exchange(0.0, std::memory_order_acq_rel);
    out.wheel       = st.accum_wheel.exchange(0.0, std::memory_order_acq_rel);
    out.x           = st.x.load(std::memory_order_relaxed);
    out.y           = st.y.load(std::memory_order_relaxed);
    out.left_down   = st.left_down.load(std::memory_order_relaxed);
    out.right_down  = st.right_down.load(std::memory_order_relaxed);
    out.middle_down = st.middle_down.load(std::memory_order_relaxed);
    return out;
}

} // namespace psynder::platform::macos

// ─── Objective-C: window delegate + Metal-layer-backed view ─────────────
@interface PsynderGxWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) std::atomic<bool>* shouldClose;
@end

@implementation PsynderGxWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (self.shouldClose) {
        self.shouldClose->store(true, std::memory_order_relaxed);
    }
    return NO; // We close the window ourselves on shutdown.
}
@end

@interface PsynderGxMetalView : NSView
- (CAMetalLayer*)metalLayer;
@end

@implementation PsynderGxMetalView
// Layer-backed view: AppKit calls -makeBackingLayer; our return value
// becomes `self.layer`. We do NOT override +layerClass because that path
// only fires for views created with -setWantsLayer:YES *before* AppKit
// has decided to use the default makeBackingLayer; supplying both was
// redundant and risked the default CALayer winning the race when the
// view re-encodes itself.
- (BOOL)wantsUpdateLayer    { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView    { return YES; }
- (BOOL)isOpaque            { return YES; }

- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer       = [CAMetalLayer layer];
    layer.opaque              = YES;
    layer.framebufferOnly     = YES;       // overridden by lane 07 if it wants compute writes
    layer.presentsWithTransaction = NO;
    // pixelFormat + device are intentionally left for lane 07 to set so the
    // GPU lane owns colour-space / swapchain-format policy. We seed with a
    // safe default in case the lane never gets around to it.
    layer.pixelFormat         = MTLPixelFormatBGRA8Unorm;
    return layer;
}

- (CAMetalLayer*)metalLayer {
    return static_cast<CAMetalLayer*>(self.layer);
}

// Drawable-size sync on resize. AppKit fires -viewDidChangeBackingProperties
// when the window moves between displays of different scale factors, and
// -setFrameSize: when the user drags the resize handle.
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self syncDrawableSize];
}
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self syncDrawableSize];
}

- (void)syncDrawableSize {
    CAMetalLayer* layer = [self metalLayer];
    if (!layer) return;
    CGFloat scale = self.window ? self.window.backingScaleFactor : 1.0;
    layer.contentsScale = scale;
    NSSize sz = self.bounds.size;
    CGFloat dw = std::max<CGFloat>(sz.width  * scale, 1.0);
    CGFloat dh = std::max<CGFloat>(sz.height * scale, 1.0);
    layer.drawableSize  = CGSizeMake(dw, dh);
}
@end

// ─── Window impl + C++ public surface ───────────────────────────────────
namespace psynder::platform::macos {

struct Window {
    NSWindow*                ns_window     = nil;
    PsynderGxMetalView*      view          = nil;
    CAMetalLayer*            layer         = nil;
    PsynderGxWindowDelegate* delegate      = nil;
    std::atomic<bool>        should_close{false};
    WindowDesc               desc{};
};

Window* create_window(const WindowDesc& desc) {
    ensure_app_initialised();

    Window* w = new Window();
    w->desc = desc;

    @autoreleasepool {
        NSRect frame = NSMakeRect(0, 0, desc.window_width, desc.window_height);
        NSWindowStyleMask style =
            NSWindowStyleMaskTitled       |
            NSWindowStyleMaskClosable     |
            NSWindowStyleMaskMiniaturizable;
        if (desc.resizable) style |= NSWindowStyleMaskResizable;

        w->ns_window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:style
                        backing:NSBackingStoreBuffered
                          defer:NO];

        NSString* title = [NSString stringWithUTF8String:(desc.title ? desc.title : "Psynder-GX")];
        [w->ns_window setTitle:title];
        [w->ns_window setReleasedWhenClosed:NO];
        [w->ns_window center];

        // Close-button bridge
        PsynderGxWindowDelegate* del = [[PsynderGxWindowDelegate alloc] init];
        del.shouldClose = &w->should_close;
        w->delegate = del;
        [w->ns_window setDelegate:del];

        // Metal-layer-backed view
        w->view = [[PsynderGxMetalView alloc] initWithFrame:frame];
        [w->view setWantsLayer:YES];
        [w->ns_window setContentView:w->view];
        [w->ns_window makeFirstResponder:w->view];
        [w->ns_window setAcceptsMouseMovedEvents:YES];

        // Force AppKit to materialise the backing layer NOW so lane 07 can
        // pick it up immediately after create_window returns. Without this,
        // makeBackingLayer doesn't fire until the first display cycle.
        (void)[w->view metalLayer];
        w->layer = [w->view metalLayer];
        if (w->layer) {
            CGFloat scale = desc.high_dpi ? [w->ns_window backingScaleFactor] : 1.0;
            w->layer.contentsScale = scale;
            w->layer.drawableSize  = CGSizeMake(
                std::max<CGFloat>(desc.window_width  * scale, 1.0),
                std::max<CGFloat>(desc.window_height * scale, 1.0));
        }

        [w->ns_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }

    // Arm input pipelines. IOKit can soft-fail (Input Monitoring permission
    // gate on Catalina+); that's fine — NSEvent mouseMoved is the fallback.
    (void)iokit_mouse_arm();
    gamepad_arm();

    return w;
}

void destroy_window(Window* w) {
    if (!w) return;
    @autoreleasepool {
        if (w->ns_window) {
            [w->ns_window setDelegate:nil];
            [w->ns_window orderOut:nil];
            [w->ns_window close];
        }
        w->ns_window = nil;
        w->view      = nil;
        w->layer     = nil;
        w->delegate  = nil;
    }
    delete w;
}

void* native_window(Window* w) { return w ? (__bridge void*)w->ns_window : nullptr; }
void* native_layer (Window* w) { return w ? (__bridge void*)w->layer     : nullptr; }
void* native_view  (Window* w) { return w ? (__bridge void*)w->view      : nullptr; }

void* create_metal_layer(void* ns_window_ptr) {
    if (!ns_window_ptr) return nullptr;
    NSWindow* ns_window = (__bridge NSWindow*)ns_window_ptr;

    // If the window already has a layer-backed view whose layer is a
    // CAMetalLayer, return that — re-attaching would orphan the existing
    // surface and confuse any caller already drawing into it.
    NSView* existing = [ns_window contentView];
    if (existing && existing.layer && [existing.layer isKindOfClass:[CAMetalLayer class]]) {
        return (__bridge void*)existing.layer;
    }

    @autoreleasepool {
        NSRect frame = existing ? existing.bounds : [ns_window frame];
        PsynderGxMetalView* view = [[PsynderGxMetalView alloc] initWithFrame:frame];
        [view setWantsLayer:YES];
        [ns_window setContentView:view];
        // makeBackingLayer fires on first access of .layer
        (void)view.layer;
        CAMetalLayer* layer = [view metalLayer];
        if (layer) {
            CGFloat scale = [ns_window backingScaleFactor];
            layer.contentsScale = scale;
            layer.drawableSize  = CGSizeMake(
                std::max<CGFloat>(frame.size.width  * scale, 1.0),
                std::max<CGFloat>(frame.size.height * scale, 1.0));
        }
        return (__bridge void*)layer;
    }
}

std::uint32_t drawable_width(Window* w) {
    if (!w || !w->layer) return 0;
    return static_cast<std::uint32_t>(w->layer.drawableSize.width);
}
std::uint32_t drawable_height(Window* w) {
    if (!w || !w->layer) return 0;
    return static_cast<std::uint32_t>(w->layer.drawableSize.height);
}
std::uint32_t point_width(Window* w) {
    if (!w || !w->view) return 0;
    return static_cast<std::uint32_t>(w->view.bounds.size.width);
}
std::uint32_t point_height(Window* w) {
    if (!w || !w->view) return 0;
    return static_cast<std::uint32_t>(w->view.bounds.size.height);
}

bool should_close(Window* w) {
    return w ? w->should_close.load(std::memory_order_relaxed) : true;
}
void request_close(Window* w) {
    if (w) w->should_close.store(true, std::memory_order_relaxed);
}

void update_mouse_position_from_event(NSEvent* event) {
    NSWindow* window = event.window;
    if (!window) return;
    NSView* view = window.contentView;
    if (!view) return;

    NSPoint p = [view convertPoint:event.locationInWindow fromView:nil];
    const CGFloat h = view.bounds.size.height;
    auto& st = raw_mouse_state();
    st.x.store(static_cast<double>(p.x), std::memory_order_relaxed);
    st.y.store(static_cast<double>(std::max<CGFloat>(0.0, h - p.y)),
               std::memory_order_relaxed);
}

// Active window pointer for Esc → request_close. Set by run_loop; apps that
// drive their own event loop should call set_esc_close_target() after
// create_window if they want keyboard-Esc to dismiss the window.
static Window* g_esc_close_target = nullptr;

Window* set_esc_close_target(Window* w) {
    Window* prev = g_esc_close_target;
    g_esc_close_target = w;
    return prev;
}

// FPS mouse capture. Hides the OS cursor and decouples it from the physical
// mouse (CGAssociateMouseAndMouseCursorPosition(false)) so the pointer never
// drifts off-window or hits a screen edge. While captured, relative motion is
// sourced from NSEvent.deltaX/deltaY (see pump_events) since the cursor
// position is frozen. Idempotent; pass false to restore.
void set_mouse_captured(bool captured) {
    if (captured == g_mouse_captured.load(std::memory_order_relaxed)) return;
    g_mouse_captured.store(captured, std::memory_order_relaxed);
    if (captured) {
        CGDisplayHideCursor(kCGDirectMainDisplay);
        CGAssociateMouseAndMouseCursorPosition(false);
    } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        CGDisplayShowCursor(kCGDirectMainDisplay);
    }
}

void pump_events() {
    // Clear edge-triggered pressed[] at frame boundary BEFORE dispatching
    // new events so pressed[] reflects only this frame's transitions.
    keyboard_begin_frame();

    @autoreleasepool {
        NSEvent* event;
        do {
            event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES];
            if (!event) break;

            const NSEventType type = event.type;
            bool handled_by_engine = false;

            if (type == NSEventTypeKeyDown || type == NSEventTypeKeyUp) {
                // keyCode is a uint16_t virtual keycode (HIToolbox constants).
                const uint16_t raw_vk = event.keyCode;
                const platform::KeyCode kc = vk_to_keycode(raw_vk);
                const bool is_down = (type == NSEventTypeKeyDown);
                keyboard_event(kc, is_down);
                const bool command_modified =
                    (event.modifierFlags & NSEventModifierFlagCommand) != 0;
                if (is_down && !command_modified) {
                    append_text_input(event.characters);
                } else if (is_down && command_modified) {
                    capture_command_shortcut(event);
                }
                // Esc: signal the active window to close.
                if (is_down && kc == platform::KeyCode::Escape && g_esc_close_target) {
                    request_close(g_esc_close_target);
                }
                // Game/text input is consumed by the engine. Forwarding plain
                // keyDown events into AppKit makes NSWindow beep because the
                // CAMetalLayer view has no native text responder.
                handled_by_engine = !command_modified ||
                    g_console_shortcuts.copy ||
                    g_console_shortcuts.cut ||
                    g_console_shortcuts.paste ||
                    g_console_shortcuts.select_all;
            } else if (type == NSEventTypeFlagsChanged) {
                // Modifier keys use FlagsChanged instead of KeyDown/KeyUp.
                // Determine down/up by testing the relevant modifier flag.
                const uint16_t raw_vk = event.keyCode;
                const platform::KeyCode kc = vk_to_keycode(raw_vk);
                const NSEventModifierFlags mods = event.modifierFlags;
                bool is_down = false;
                // Map each modifier virtual keycode to its flag bit.
                switch (raw_vk) {
                    case vk::kLeftShift:  case vk::kRightShift:
                        is_down = (mods & NSEventModifierFlagShift)   != 0; break;
                    case vk::kLeftCtrl:   case vk::kRightCtrl:
                        is_down = (mods & NSEventModifierFlagControl) != 0; break;
                    case vk::kLeftAlt:    case vk::kRightAlt:
                        is_down = (mods & NSEventModifierFlagOption)  != 0; break;
                    default: break;
                }
                keyboard_event(kc, is_down);
                handled_by_engine = true;
            } else if (type == NSEventTypeScrollWheel) {
                update_mouse_position_from_event(event);
                add_atomic_double(raw_mouse_state().accum_wheel,
                                  static_cast<double>(event.scrollingDeltaY));
                handled_by_engine = true;
            } else if (type == NSEventTypeMouseMoved ||
                       type == NSEventTypeLeftMouseDragged ||
                       type == NSEventTypeRightMouseDragged ||
                       type == NSEventTypeOtherMouseDragged) {
                update_mouse_position_from_event(event);
                // While captured, the cursor position is frozen
                // (CGAssociateMouseAndMouseCursorPosition(false)), so derive
                // the relative motion from NSEvent's hardware deltas instead.
                // Same sign convention as the IOKit HID path (right/down
                // positive) and the same point scale as the cursor-position
                // fallback, so aim sensitivity is unchanged.
                if (g_mouse_captured.load(std::memory_order_relaxed)) {
                    add_atomic_double(raw_mouse_state().accum_dx,
                                      static_cast<double>(event.deltaX));
                    add_atomic_double(raw_mouse_state().accum_dy,
                                      static_cast<double>(event.deltaY));
                }
                handled_by_engine = true;
            } else if (type == NSEventTypeLeftMouseDown ||
                       type == NSEventTypeLeftMouseUp ||
                       type == NSEventTypeRightMouseDown ||
                       type == NSEventTypeRightMouseUp ||
                       type == NSEventTypeOtherMouseDown ||
                       type == NSEventTypeOtherMouseUp) {
                update_mouse_position_from_event(event);
                auto& st = raw_mouse_state();
                if (type == NSEventTypeLeftMouseDown) {
                    st.left_down.store(true, std::memory_order_relaxed);
                } else if (type == NSEventTypeLeftMouseUp) {
                    st.left_down.store(false, std::memory_order_relaxed);
                } else if (type == NSEventTypeRightMouseDown) {
                    st.right_down.store(true, std::memory_order_relaxed);
                } else if (type == NSEventTypeRightMouseUp) {
                    st.right_down.store(false, std::memory_order_relaxed);
                } else if (type == NSEventTypeOtherMouseDown) {
                    st.middle_down.store(true, std::memory_order_relaxed);
                } else if (type == NSEventTypeOtherMouseUp) {
                    st.middle_down.store(false, std::memory_order_relaxed);
                }
                handled_by_engine = true;
            }

            if (!handled_by_engine) {
                [NSApp sendEvent:event];
            }
        } while (true);
        [NSApp updateWindows];
    }
}

std::size_t text_input_utf8(char* dst, std::size_t capacity) {
    if (!dst || capacity == 0) {
        return 0;
    }
    const std::size_t n = std::min(g_text_input_size, capacity - 1u);
    if (n > 0) {
        std::memcpy(dst, g_text_input, n);
    }
    dst[n] = '\0';
    return n;
}

ConsoleShortcuts consume_console_shortcuts() {
    g_console_shortcuts.paste_text = g_console_paste_text.c_str();
    ConsoleShortcuts out = g_console_shortcuts;
    g_console_shortcuts = {};
    return out;
}

void set_clipboard_text(const char* text) {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    NSString* s = [NSString stringWithUTF8String:(text ? text : "")];
    [pb setString:s forType:NSPasteboardTypeString];
}

std::uint64_t run_loop(Window* window, FrameCallback frame, void* frame_user) {
    if (!window) return 0;
    // Register the window so Esc fires request_close from inside pump_events.
    Window* prev_target = set_esc_close_target(window);
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    std::uint64_t frames = 0;
    while (!should_close(window)) {
        pump_events();
        auto now = clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (frame) frame(frame_user, window, dt);
        ++frames;
    }
    set_esc_close_target(prev_target);
    return frames;
}

MouseDelta mouse_delta_raw() {
    // mouse_delta_raw returns deltas accumulated since the LAST pump_events
    // reset (or the last call to this function in mid-frame, if the caller
    // is polling sub-frame). The snapshot-and-reset semantics mean the
    // counter zeros each read, so an FPS loop polling 4x per frame gets
    // four chunks summing to the per-frame total.
    RawMouseSnapshot s = raw_mouse_snapshot_and_reset();
    MouseDelta out{};
    out.dx          = s.dx;
    out.dy          = s.dy;
    out.wheel       = s.wheel;
    out.x           = s.x;
    out.y           = s.y;
    out.left_down   = s.left_down;
    out.right_down  = s.right_down;
    out.middle_down = s.middle_down;
    return out;
}

// ─── Gamepad ────────────────────────────────────────────────────────────
namespace {
struct GamepadState {
    std::atomic<std::uint32_t> count{0};
    std::atomic<bool>          armed{false};
};
GamepadState& gp_state() {
    static GamepadState s;
    return s;
}
} // namespace

void gamepad_arm() {
    auto& gp = gp_state();
    if (gp.armed.exchange(true)) return;
    @autoreleasepool {
        [[NSNotificationCenter defaultCenter]
            addObserverForName:GCControllerDidConnectNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification* /*note*/) {
            gp_state().count.fetch_add(1, std::memory_order_relaxed);
        }];
        [[NSNotificationCenter defaultCenter]
            addObserverForName:GCControllerDidDisconnectNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification* /*note*/) {
            // Saturating decrement.
            auto cur = gp_state().count.load(std::memory_order_relaxed);
            while (cur > 0 && !gp_state().count.compare_exchange_weak(
                                  cur, cur - 1, std::memory_order_relaxed)) {}
        }];
        gp.count.store(static_cast<std::uint32_t>([[GCController controllers] count]),
                       std::memory_order_relaxed);
    }
}

std::uint32_t gamepad_count() {
    return gp_state().count.load(std::memory_order_relaxed);
}

// ─── CoreAudio device-name lookup (lane 14 owns the mixer) ──────────────
namespace {

std::string& audio_name_cache() {
    static std::string s;
    return s;
}

AudioDeviceID default_output_device_id() {
    AudioObjectPropertyAddress addr{
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    AudioDeviceID dev = kAudioObjectUnknown;
    UInt32        size = sizeof(dev);
    OSStatus rc = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                             &addr, 0, nullptr, &size, &dev);
    if (rc != noErr) return kAudioObjectUnknown;
    return dev;
}

} // namespace

const char* default_audio_device_name() {
    auto& cache = audio_name_cache();
    AudioDeviceID dev = default_output_device_id();
    if (dev == kAudioObjectUnknown) {
        cache = "<no-default-output>";
        return cache.c_str();
    }
    AudioObjectPropertyAddress addr{
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    CFStringRef name = nullptr;
    UInt32      size = sizeof(name);
    OSStatus rc = AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &name);
    if (rc != noErr || !name) {
        cache = "<unnamed-output>";
        return cache.c_str();
    }
    // CFStringGetCStringPtr can return nullptr if the encoding mismatches;
    // fall back to GetCString into a stack buffer.
    char buf[256] = {0};
    if (!CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8)) {
        std::strncpy(buf, "<utf8-conv-failed>", sizeof(buf) - 1);
    }
    CFRelease(name);
    cache.assign(buf);
    return cache.c_str();
}

std::uint32_t default_audio_sample_rate() {
    AudioDeviceID dev = default_output_device_id();
    if (dev == kAudioObjectUnknown) return 0;
    AudioObjectPropertyAddress addr{
        kAudioDevicePropertyNominalSampleRate,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain,
    };
    Float64 sr = 0.0;
    UInt32  size = sizeof(sr);
    OSStatus rc = AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &sr);
    if (rc != noErr) return 0;
    return static_cast<std::uint32_t>(sr);
}

} // namespace psynder::platform::macos
