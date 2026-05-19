// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/mtl/MetalBackend.mm
//
// Native Metal backend for Psynder-GX on macOS Apple Silicon.
//
// Wave-B M0 goal: animated clear-color via Metal. The MTLDevice + a
// CAMetalLayer attached to an NSWindow's content view (provided by
// lane 25 platform-macos via DeviceDesc::native_window_handle).
//
// Frame loop:
//   begin_frame:  nextDrawable from the layer; create command buffer.
//   cmd_open:     return our single CmdBuffer wrapper (one per worker
//                 will land at M1; for M0 one is enough).
//   cmd_submit:   encode a clear-pass against the drawable. Color comes
//                 from device->current_frame_index so we get animation
//                 even without sample-side rendering code.
//   end_frame:    presentDrawable + commit + wait (single buffered).
//
// Apple Silicon is unified memory — we report device_is_unified_memory()
// = true so future lanes can skip the staging-upload path (DESIGN §4.2).
//
// NO mid-frame allocations. CommandBuffer is the only per-frame "new"
// API call, and Apple's pool reuses storage. Drawables are owned by the
// CAMetalLayer pool.

#include "gpu/PublicGpu.h"
#include "gpu/PublicGpuInternal.h"
#include "gpu/mtl/MetalBackend.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AppKit/AppKit.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace psynder::gpu {

// ─── Concrete Metal-owning resource types ───────────────────────────────
struct MtlBuffer  : Buffer  { id<MTLBuffer>       handle = nil; void* mapped = nullptr; };
struct MtlTexture : Texture { id<MTLTexture>      handle = nil; };
struct MtlSampler : Sampler { id<MTLSamplerState> handle = nil; };
struct MtlCmdBuf  : CmdBuffer {
    id<MTLCommandBuffer>          cb      = nil;
    id<MTLRenderCommandEncoder>   encoder = nil;
};

namespace mtl {

// Per-frame state that crosses begin_frame -> end_frame -> cmd_open.
// Lives inside the .mm since the id<T> members need Foundation visible.
struct FrameState {
    id<CAMetalDrawable>  drawable        = nil;
    id<MTLCommandBuffer> command_buffer  = nil;
    bool                 encoded_clear   = false;
};

// ─── Backend instance ───────────────────────────────────────────────────
class MetalBackend final : public Backend {
public:
    bool init(Device* dev) override;
    void shutdown(Device* dev) override;

    bool begin_frame(Device* dev) override;
    void end_frame(Device* dev) override;

    CmdBuffer* cmd_open(Device* dev) override;
    void       cmd_submit(Device* dev, CmdBuffer* cmd) override;

    void resize_swapchain(Device* dev, std::uint32_t w, std::uint32_t h) override;

    Buffer*  create_buffer (Device* dev, const BufferDesc& d) override;
    Texture* create_texture(Device* dev, const TextureDesc& d) override;
    Sampler* create_sampler(Device* dev) override;

    void*    buffer_map  (Buffer* b) override;
    void     buffer_unmap(Buffer* b) override;

    void destroy_resource(RefCountedBase* res) override;

private:
    bool attach_layer_from_handle_(Device* dev, void* native_handle);

    id<MTLDevice>          mtl_device_   = nil;
    id<MTLCommandQueue>    queue_        = nil;
    CAMetalLayer*          layer_        = nil;

    // Per-frame transient state. Cleared at end_frame.
    FrameState frame_{};

    // The single CmdBuffer at M0 — one worker, one buffer. Reused across
    // frames. M1+: pool of these, one per worker thread, drawn from a
    // per-frame ring.
    MtlCmdBuf  cmd_storage_{};

    // Cached device name (Metal returns NSString, we want const char*).
    char device_name_buf_[256] = {0};
};

Backend* make_metal_backend() { return new (std::nothrow) MetalBackend(); }

// ─── init / shutdown ────────────────────────────────────────────────────
bool MetalBackend::init(Device* dev) {
    @autoreleasepool {
        mtl_device_ = MTLCreateSystemDefaultDevice();
        if (!mtl_device_) {
            std::fputs("[psy::gpu::mtl] MTLCreateSystemDefaultDevice returned nil\n", stderr);
            return false;
        }

        queue_ = [mtl_device_ newCommandQueue];
        if (!queue_) {
            std::fputs("[psy::gpu::mtl] newCommandQueue returned nil\n", stderr);
            return false;
        }

        // Cache device name as a C string for the public API.
        NSString* name = [mtl_device_ name];
        const char* utf = [name UTF8String];
        if (utf) {
            std::strncpy(device_name_buf_, utf, sizeof(device_name_buf_) - 1);
        } else {
            std::strncpy(device_name_buf_, "Apple GPU", sizeof(device_name_buf_) - 1);
        }
        dev->device_name_cstr = device_name_buf_;

        // Apple Silicon is always unified memory. Apple Intel discrete
        // GPUs (deprecated) are not, but Psynder-GX targets Apple Silicon
        // only — the platform check at root CMakeLists enforces it.
        dev->unified_memory = [mtl_device_ hasUnifiedMemory];

        // RT + mesh-shader capabilities. M3 and newer Apple Silicon
        // chips support Metal 4 RT and object/mesh stages.
        dev->supports_rt = (bool)[mtl_device_ supportsRaytracing];
        // Apple's mesh-shader API is supportsFamily:MTLGPUFamilyMetal3.
        dev->supports_mesh = [mtl_device_ supportsFamily:MTLGPUFamilyMetal3];

        // If the platform lane passed us a CAMetalLayer / NSWindow*,
        // attach it. If not, run "headless" (no swapchain) — the M0
        // sample will hand us a real one, but lane bring-up smoke
        // tests don't need a visible surface.
        if (dev->desc.native_window_handle) {
            if (!attach_layer_from_handle_(dev, dev->desc.native_window_handle)) {
                std::fputs("[psy::gpu::mtl] failed to attach CAMetalLayer to native handle\n", stderr);
                // Not fatal — the M0 sample may construct the device,
                // print a banner, then immediately destroy it for smoke
                // mode. The platform lane should always pass a valid
                // layer in interactive mode.
            }
        }

        // Initial swapchain extent (placeholder until first resize call).
        dev->swapchain_width  = layer_ ? (std::uint32_t)layer_.drawableSize.width  : 0u;
        dev->swapchain_height = layer_ ? (std::uint32_t)layer_.drawableSize.height : 0u;

        // Stash the single per-worker CmdBuffer's owner so destroy_resource
        // sees a valid device pointer should anything go wrong.
        cmd_storage_.set_owner(dev);

        std::printf("[psy::gpu::mtl] init: device=\"%s\" unified=%d rt=%d mesh=%d layer=%p\n",
                    device_name_buf_, (int)dev->unified_memory,
                    (int)dev->supports_rt, (int)dev->supports_mesh,
                    (void*)layer_);
        return true;
    } // @autoreleasepool
}

void MetalBackend::shutdown(Device* /*dev*/) {
    @autoreleasepool {
        if (queue_) {
            [queue_ release];
            queue_ = nil;
        }
        if (mtl_device_) {
            [mtl_device_ release];
            mtl_device_ = nil;
        }
        // CAMetalLayer is owned by the platform lane (the NSView's layer).
        // We don't release it.
        layer_ = nil;
    }
}

// ─── Layer attach ───────────────────────────────────────────────────────
//
// The native_window_handle may be:
//   * A CAMetalLayer* directly (preferred — lane 25 sets up MTKView/MTLView
//     with .wantsLayer + a CAMetalLayer sublayer).
//   * An NSWindow* — we walk to contentView and install a CAMetalLayer on it.
//   * An NSView*    — we install a CAMetalLayer on it.
//
// We probe via runtime isKindOfClass: to avoid hard-coding lane 25's choice.
bool MetalBackend::attach_layer_from_handle_(Device* dev, void* native_handle) {
    @autoreleasepool {
        id obj = (__bridge id)native_handle;
        if (!obj) return false;

        CAMetalLayer* layer = nil;

        if ([obj isKindOfClass:[CAMetalLayer class]]) {
            layer = (CAMetalLayer*)obj;
        } else if ([obj isKindOfClass:[NSView class]]) {
            NSView* view = (NSView*)obj;
            if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
                layer = (CAMetalLayer*)view.layer;
            } else {
                // Install a fresh CAMetalLayer on the view.
                layer = [CAMetalLayer layer];
                view.wantsLayer = YES;
                view.layer      = layer;
            }
        } else if ([obj isKindOfClass:[NSWindow class]]) {
            NSWindow* window = (NSWindow*)obj;
            NSView* view = window.contentView;
            if (!view) return false;
            if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
                layer = (CAMetalLayer*)view.layer;
            } else {
                layer = [CAMetalLayer layer];
                view.wantsLayer = YES;
                view.layer      = layer;
            }
        } else {
            std::fputs("[psy::gpu::mtl] native_window_handle is neither CAMetalLayer/NSView/NSWindow\n", stderr);
            return false;
        }

        if (!layer) return false;

        layer.device         = mtl_device_;
        layer.pixelFormat    = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.opaque          = YES;
        // Allow next-drawable timeout to be short — competitive FPS
        // engine, we'd rather skip a frame than stall.
        if ([layer respondsToSelector:@selector(setAllowsNextDrawableTimeout:)]) {
            layer.allowsNextDrawableTimeout = YES;
        }

        layer_ = layer;
        // Bump the drawable size to match the view's pixel-backing if
        // possible (handles Retina scale automatically).
        if ([obj isKindOfClass:[NSView class]] || [obj isKindOfClass:[NSWindow class]]) {
            NSView* view = ([obj isKindOfClass:[NSWindow class]])
                ? ((NSWindow*)obj).contentView
                : (NSView*)obj;
            CGFloat scale = view.window.backingScaleFactor;
            if (scale <= 0) scale = 1.0;
            NSSize sz = view.bounds.size;
            layer_.drawableSize = CGSizeMake(sz.width * scale, sz.height * scale);
            dev->swapchain_width  = (std::uint32_t)layer_.drawableSize.width;
            dev->swapchain_height = (std::uint32_t)layer_.drawableSize.height;
        }
        return true;
    }
}

// ─── Frame loop ─────────────────────────────────────────────────────────
bool MetalBackend::begin_frame(Device* /*dev*/) {
    @autoreleasepool {
        if (!layer_) {
            // Headless smoke mode: nothing to do; cmd_submit becomes a no-op.
            return true;
        }
        frame_.drawable = [[layer_ nextDrawable] retain];
        if (!frame_.drawable) {
            // Hit the next-drawable timeout. Skip this frame; render lanes
            // should treat begin_frame() == false as "drop this frame".
            return false;
        }
        frame_.command_buffer = [[queue_ commandBuffer] retain];
        frame_.command_buffer.label = @"psy::gpu frame";
        frame_.encoded_clear = false;
        return true;
    }
}

void MetalBackend::end_frame(Device* /*dev*/) {
    @autoreleasepool {
        if (!frame_.command_buffer) {
            return;
        }
        if (frame_.drawable) {
            [frame_.command_buffer presentDrawable:frame_.drawable];
        }
        [frame_.command_buffer commit];

        // Single-buffered M0: wait so we know the GPU is done. M1 will
        // run frames-in-flight = 2 via a CPU-side semaphore.
        [frame_.command_buffer waitUntilCompleted];

        [frame_.command_buffer release];
        frame_.command_buffer = nil;

        if (frame_.drawable) {
            [frame_.drawable release];
            frame_.drawable = nil;
        }
        frame_.encoded_clear = false;
    }
}

CmdBuffer* MetalBackend::cmd_open(Device* /*dev*/) {
    // Single-buffer storage for M0. Reset the flags.
    cmd_storage_.open      = true;
    cmd_storage_.submitted = false;
    cmd_storage_.cb        = frame_.command_buffer; // shared
    cmd_storage_.encoder   = nil;
    return &cmd_storage_;
}

// Encode an animated clear pass against the swapchain drawable. The
// color cycles with current_frame_index so the M0 sample can verify a
// live present without writing any rendering code.
static void encode_animated_clear(id<MTLCommandBuffer> cb,
                                  id<CAMetalDrawable> drawable,
                                  std::uint64_t frame_index)
{
    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture     = drawable.texture;
    rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;

    const double t = (double)frame_index * (1.0 / 60.0);   // assume 60 fps tick
    const double r = 0.5 + 0.5 * std::sin(t * 0.97);
    const double g = 0.5 + 0.5 * std::sin(t * 1.31 + 2.0);
    const double b = 0.5 + 0.5 * std::sin(t * 1.73 + 4.0);
    rpd.colorAttachments[0].clearColor  = MTLClearColorMake(r, g, b, 1.0);

    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
    enc.label = @"psy::gpu clear";
    [enc endEncoding];
}

void MetalBackend::cmd_submit(Device* dev, CmdBuffer* cmd) {
    @autoreleasepool {
        if (!cmd) return;
        cmd->open      = false;
        cmd->submitted = true;
        if (!frame_.command_buffer || !frame_.drawable) {
            return; // headless or drawable timeout
        }
        if (!frame_.encoded_clear) {
            // M0 default behavior: every cmd_submit encodes the
            // animated clear once per frame. Render lanes that submit
            // real work in M1+ will skip this default by binding their
            // own render passes.
            encode_animated_clear(frame_.command_buffer,
                                  frame_.drawable,
                                  dev->current_frame_index);
            frame_.encoded_clear = true;
        }
    }
}

void MetalBackend::resize_swapchain(Device* dev, std::uint32_t w, std::uint32_t h) {
    @autoreleasepool {
        if (!layer_ || w == 0 || h == 0) return;
        layer_.drawableSize = CGSizeMake((CGFloat)w, (CGFloat)h);
        dev->swapchain_width  = w;
        dev->swapchain_height = h;
    }
}

// ─── Resource creation — minimal stubs for M0 ───────────────────────────
//
// Buffers / textures / samplers are not exercised by sample_00_clear.
// We return concrete RefCountedBase-derived stub objects so Handle<T>
// arithmetic works for callers (lane 09/21) that wire up paths against
// the public surface but don't actually use the contents at M0.
Buffer* MetalBackend::create_buffer(Device* /*dev*/, const BufferDesc& /*desc*/) {
    return new (std::nothrow) MtlBuffer();
}
Texture* MetalBackend::create_texture(Device* /*dev*/, const TextureDesc& /*desc*/) {
    return new (std::nothrow) MtlTexture();
}
Sampler* MetalBackend::create_sampler(Device* /*dev*/) {
    return new (std::nothrow) MtlSampler();
}

void* MetalBackend::buffer_map(Buffer* b) {
    auto* mb = static_cast<MtlBuffer*>(b);
    return mb->mapped; // nullptr at M0 — full impl at M1
}
void MetalBackend::buffer_unmap(Buffer* /*b*/) {
    // no-op stub
}

void MetalBackend::destroy_resource(RefCountedBase* res) {
    // M0: synchronous destroy. M1+: defer-release to last_completed_frame.
    delete res;
}

} // namespace mtl

// ─── create_backend factory — selected at link time ─────────────────────
//
// On macOS, the lane's CMakeLists builds only the Metal sources; we are
// the sole definition of create_backend(). The Vulkan TU has its own
// create_backend() that is conditionally compiled on Win/Linux.
Backend* create_backend() {
    return mtl::make_metal_backend();
}

} // namespace psynder::gpu
