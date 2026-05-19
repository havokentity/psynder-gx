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

#include "MacosPlatform_internal.h"

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
        switch (usage) {
            case kHIDUsage_GD_X: {
                // Atomic accumulation. fetch_add isn't defined on double in
                // C++20 atomics by default for all stdlib versions; do CAS.
                double cur = st.accum_dx.load(std::memory_order_relaxed);
                while (!st.accum_dx.compare_exchange_weak(
                           cur, cur + static_cast<double>(raw),
                           std::memory_order_relaxed)) { /* retry */ }
                break;
            }
            case kHIDUsage_GD_Y: {
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

void pump_events() {
    @autoreleasepool {
        NSEvent* event;
        do {
            event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES];
            if (event) [NSApp sendEvent:event];
        } while (event);
        [NSApp updateWindows];
    }
}

std::uint64_t run_loop(Window* window, FrameCallback frame, void* frame_user) {
    if (!window) return 0;
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
