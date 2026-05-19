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
    id<MTLCommandBuffer>          cb               = nil;
    id<MTLRenderCommandEncoder>   encoder          = nil;
    id<MTLComputeCommandEncoder>  compute_encoder  = nil;
    // Cached state set by bind_pipeline. The PipelineHandle id is the
    // key into the lane 07-local Metal pipeline shim (see mtl_pipeline_*
    // helpers in MetalBackend.mm). Null when no pipeline bound.
    id<MTLRenderPipelineState>    bound_render_pso = nil;
    id<MTLComputePipelineState>   bound_compute_pso = nil;
    MTLSize                       compute_tgs      = MTLSizeMake(1, 1, 1);
    // Cached index binding consumed by drawIndexedPrimitives.
    id<MTLBuffer>                 index_mtl        = nil;
    MTLIndexType                  index_mtl_type   = MTLIndexTypeUInt16;
    std::uint64_t                 index_mtl_offset = 0;
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

    // Render-encoder API.
    void begin_render(CmdBuffer*, const RenderPassDesc&) override;
    void end_render  (CmdBuffer*) override;
    void set_viewport(CmdBuffer*, const Viewport&) override;
    void set_scissor (CmdBuffer*, const Scissor&)  override;
    void bind_pipeline     (CmdBuffer*, ::psynder::shader::PipelineHandle) override;
    void bind_vertex_buffer(CmdBuffer*, std::uint32_t, Buffer*, std::uint64_t) override;
    void bind_index_buffer (CmdBuffer*, Buffer*, IndexType, std::uint64_t) override;
    void push_constants    (CmdBuffer*, const void*, std::uint32_t, std::uint32_t) override;
    void draw        (CmdBuffer*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override;
    void draw_indexed(CmdBuffer*, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t) override;
    void dispatch    (CmdBuffer*, std::uint32_t, std::uint32_t, std::uint32_t) override;

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
    cmd_storage_.open              = true;
    cmd_storage_.submitted         = false;
    cmd_storage_.cb                = frame_.command_buffer; // shared
    cmd_storage_.encoder           = nil;
    cmd_storage_.compute_encoder   = nil;
    cmd_storage_.bound_render_pso  = nil;
    cmd_storage_.bound_compute_pso = nil;
    cmd_storage_.index_mtl         = nil;
    cmd_storage_.index_mtl_offset  = 0;
    cmd_storage_.index_mtl_type    = MTLIndexTypeUInt16;
    cmd_storage_.compute_tgs       = MTLSizeMake(1, 1, 1);
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
        auto* c = static_cast<MtlCmdBuf*>(cmd);
        // Close any encoder the user left open. Render lanes that call
        // begin_render but forget end_render still get a valid submit.
        // Render encoder carries an explicit [retain] from begin_render —
        // balance it with [release] here on the forgotten-end_render path.
        // (Compute encoders are opened-and-closed within a single dispatch()
        // autoreleasepool scope and never persist across calls, so they
        // need only endEncoding here, not release.)
        if (c->encoder) {
            [c->encoder endEncoding];
            [c->encoder release];
            c->encoder = nil;
        }
        if (c->compute_encoder) {
            [c->compute_encoder endEncoding];
            c->compute_encoder = nil;
        }
        cmd->open      = false;
        cmd->submitted = true;
        if (!frame_.command_buffer || !frame_.drawable) {
            return; // headless or drawable timeout
        }
        // Skip the M0 auto-clear when the caller has already encoded their
        // own render pass (or compute work that flipped frame_.encoded_clear).
        // begin_render / dispatch both flip frame_.encoded_clear true.
        if (!frame_.encoded_clear && !cmd->encoded_user_render) {
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

// ─── Render-encoder API (lane09-001 unblock) ────────────────────────────
//
// Cross-lane handshake: lane 08's psynder::shader::PipelineHandle is a
// uint32_t id keyed in its local registry — it does NOT yet expose the
// underlying MTLRenderPipelineState. Lane 07 maintains a small shim
// registry of (PipelineHandle.id → id<MTLRenderPipelineState>) that
// lane 08 populates once it grows the build step. Until lane 08 starts
// publishing pipelines through psynder::gpu::mtl::register_render_pso()
// (see header below), bind_pipeline is effectively a no-op — the encoder
// still runs (set_viewport / draw etc. are valid against a nil PSO; they
// won't render but won't crash either). Issue against lane 08 captures
// the registration ABI.

namespace pipeline_shim {

// Single-process registry, populated by lane 08 once it grows Metal PSO
// emission. Keyed by PipelineHandle.id (uint32_t). Held in a flat array
// because PipelineHandle ids are dense (lane 08 hands them out 1, 2, 3...).
// Max 64 entries is a generous M1/M2 ceiling — bumping is a one-line edit.
//
// Why plain `id` (not id<MTLRenderPipelineState>): Clang's Obj-C parser
// rejects parameterized id<protocol> inside nested namespaces in some
// builds (no template named 'id'). The shim only stores + forwards
// references — type-safety is enforced at the bind site via static_cast.
constexpr std::uint32_t kMaxPipelines = 64;

struct Entry {
    // Field name `handle_id` (not `id`) so the Obj-C `id` keyword isn't
    // shadowed in nested member-init contexts.
    std::uint32_t handle_id   = 0;
    id            render_pso  = nil;   // id<MTLRenderPipelineState>
    id            compute_pso = nil;   // id<MTLComputePipelineState>
    MTLSize       compute_tgs = MTLSizeMake(1, 1, 1);
};

static Entry  g_entries[kMaxPipelines] = {};
static std::uint32_t g_count = 0;

static Entry* find(std::uint32_t needle) {
    for (std::uint32_t i = 0; i < g_count; ++i) {
        if (g_entries[i].handle_id == needle) return &g_entries[i];
    }
    return nullptr;
}

} // namespace pipeline_shim

// Public-but-lane-internal hooks lane 08 will call once it ships Metal
// pipeline-state emission. Declared `extern "C"` to keep the symbol
// stable across lane builds without dragging mtl-specific headers into
// lane 08's TU. Lane 08 declares the prototype locally + links.
extern "C" {

void psynder_gx_mtl_register_render_pso(std::uint32_t handle_id, void* mtl_pso) {
    auto& s = pipeline_shim::g_entries;
    auto& cnt = pipeline_shim::g_count;
    // ARC is OFF for this TU (per CMakeLists: -fno-objc-arc), so we can
    // bridge-cast a raw void* to a strong reference and assign without
    // ownership semantics getting in the way. The caller (lane 08) must
    // ensure the MTLRenderPipelineState outlives the registration.
    id pso = (__bridge id)mtl_pso;
    if (auto* e = pipeline_shim::find(handle_id)) {
        e->render_pso = pso;
        return;
    }
    if (cnt >= pipeline_shim::kMaxPipelines) {
        std::fputs("[psy::gpu::mtl] pipeline_shim: too many registrations\n", stderr);
        return;
    }
    s[cnt].handle_id  = handle_id;
    s[cnt].render_pso = pso;
    ++cnt;
}

void psynder_gx_mtl_register_compute_pso(std::uint32_t handle_id, void* mtl_pso,
                                         std::uint32_t tgs_x,
                                         std::uint32_t tgs_y,
                                         std::uint32_t tgs_z) {
    auto& s = pipeline_shim::g_entries;
    auto& cnt = pipeline_shim::g_count;
    id pso = (__bridge id)mtl_pso;
    MTLSize tgs = MTLSizeMake(tgs_x ? tgs_x : 1,
                              tgs_y ? tgs_y : 1,
                              tgs_z ? tgs_z : 1);
    if (auto* e = pipeline_shim::find(handle_id)) {
        e->compute_pso = pso;
        e->compute_tgs = tgs;
        return;
    }
    if (cnt >= pipeline_shim::kMaxPipelines) {
        std::fputs("[psy::gpu::mtl] pipeline_shim: too many registrations\n", stderr);
        return;
    }
    s[cnt].handle_id   = handle_id;
    s[cnt].compute_pso = pso;
    s[cnt].compute_tgs = tgs;
    ++cnt;
}

} // extern "C"

namespace {

MTLLoadAction to_mtl_load(LoadOp op) {
    switch (op) {
        case LoadOp::Load:     return MTLLoadActionLoad;
        case LoadOp::Clear:    return MTLLoadActionClear;
        case LoadOp::DontCare: return MTLLoadActionDontCare;
    }
    return MTLLoadActionClear;
}

MTLStoreAction to_mtl_store(StoreOp op) {
    switch (op) {
        case StoreOp::Store:    return MTLStoreActionStore;
        case StoreOp::DontCare: return MTLStoreActionDontCare;
    }
    return MTLStoreActionStore;
}

// Reserved Metal buffer index for inline push constants. We use slot 0
// per the orchestrator brief; vertex buffers must therefore live at
// kVertexBufferSlot0..N (defined below). Lane 09 will publish a matching
// convention in its slang shaders (forward.slang uses [[buffer(0)]] for
// the per-frame uniforms today).
constexpr NSUInteger kPushConstantSlot = 0;

// Vertex buffers from bind_vertex_buffer(binding=N) bind to Metal slot
// (kVertexBufferSlot0 + N). We offset away from slot 0 to keep the
// push-constant slot reserved. Lane 09 + lane 08 align on this.
constexpr NSUInteger kVertexBufferSlot0 = 1;

} // anonymous

void MetalBackend::begin_render(CmdBuffer* cmd, const RenderPassDesc& desc) {
    @autoreleasepool {
        auto* c = static_cast<MtlCmdBuf*>(cmd);
        if (!c || !c->cb) return;

        // Already-encoding? end_render must be called between passes.
        if (c->encoder) {
            std::fputs("[psy::gpu::mtl] begin_render called while a render encoder is live; ignored\n", stderr);
            return;
        }

        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];

        const std::uint32_t color_count = desc.color_count > 8 ? 8u : desc.color_count;
        for (std::uint32_t i = 0; i < color_count; ++i) {
            const ColorAttachment& a = desc.colors[i];
            id<MTLTexture> tex = nil;
            if (a.target == nullptr || desc.swapchain) {
                if (i == 0 && frame_.drawable) tex = frame_.drawable.texture;
            } else {
                tex = static_cast<MtlTexture*>(a.target)->handle;
            }
            if (!tex) continue;
            rpd.colorAttachments[i].texture     = tex;
            rpd.colorAttachments[i].loadAction  = to_mtl_load (a.load);
            rpd.colorAttachments[i].storeAction = to_mtl_store(a.store);
            rpd.colorAttachments[i].clearColor  = MTLClearColorMake(
                a.clear_rgba[0], a.clear_rgba[1],
                a.clear_rgba[2], a.clear_rgba[3]);
        }

        if (desc.depth.target) {
            id<MTLTexture> dtex = static_cast<MtlTexture*>(desc.depth.target)->handle;
            if (dtex) {
                rpd.depthAttachment.texture     = dtex;
                rpd.depthAttachment.loadAction  = to_mtl_load (desc.depth.load);
                rpd.depthAttachment.storeAction = to_mtl_store(desc.depth.store);
                rpd.depthAttachment.clearDepth  = desc.depth.clear_depth;
            }
        }

        // Explicit [retain] is REQUIRED here: the TU is -fno-objc-arc and
        // the call returns an autoreleased object.  Without the retain, the
        // encoder would be released when this `@autoreleasepool` block
        // drains at function exit, and every subsequent encoder call —
        // set_viewport / set_scissor / bind_pipeline / push_constants /
        // bind_vertex_buffer / bind_index_buffer / draw_indexed — would be
        // hitting a dangling pointer.  end_render() balances this with a
        // matching [release] after [endEncoding].  (Same pattern is used
        // for frame_.drawable / frame_.command_buffer above, ~L294/L300.)
        c->encoder = [[c->cb renderCommandEncoderWithDescriptor:rpd] retain];
        c->encoder.label = @"psy::gpu user pass";
        // Suppress the M0 auto-clear: cmd_submit's frame_.encoded_clear gate
        // is per-frame; flipping it here means subsequent cmd_submit
        // calls won't emit the default animated-clear pass.
        frame_.encoded_clear = true;
    }
}

void MetalBackend::end_render(CmdBuffer* cmd) {
    @autoreleasepool {
        auto* c = static_cast<MtlCmdBuf*>(cmd);
        if (!c || !c->encoder) return;
        [c->encoder endEncoding];
        // Balance the explicit [retain] in begin_render.  Without the
        // matching [release], the encoder would leak its retain count
        // indefinitely (process-wide leak — small but accumulates).
        [c->encoder release];
        c->encoder = nil;
        c->bound_render_pso = nil;
        c->index_mtl        = nil;
    }
}

void MetalBackend::set_viewport(CmdBuffer* cmd, const Viewport& vp) {
    auto* c = static_cast<MtlCmdBuf*>(cmd);
    if (!c || !c->encoder) return;
    MTLViewport v;
    v.originX = vp.x;
    v.originY = vp.y;
    v.width   = vp.w;
    v.height  = vp.h;
    v.znear   = vp.min_depth;
    v.zfar    = vp.max_depth;
    [c->encoder setViewport:v];
}

void MetalBackend::set_scissor(CmdBuffer* cmd, const Scissor& sc) {
    auto* c = static_cast<MtlCmdBuf*>(cmd);
    if (!c || !c->encoder) return;
    MTLScissorRect r;
    r.x      = (NSUInteger)(sc.x < 0 ? 0 : sc.x);
    r.y      = (NSUInteger)(sc.y < 0 ? 0 : sc.y);
    r.width  = sc.w;
    r.height = sc.h;
    [c->encoder setScissorRect:r];
}

void MetalBackend::bind_pipeline(CmdBuffer* cmd, ::psynder::shader::PipelineHandle h) {
    auto* c = static_cast<MtlCmdBuf*>(cmd);
    if (!c) return;
    if (!h.valid()) {
        c->bound_render_pso  = nil;
        c->bound_compute_pso = nil;
        return;
    }
    auto* entry = pipeline_shim::find(h.id);
    if (!entry) {
        // Lane 08 hasn't published a Metal PSO for this handle yet. This
        // is the expected state today: lane 08 only emits Metal IR blobs,
        // not state objects. Render lanes that hit this path will see
        // their pass run but produce no fragments. Logged once per
        // unique id so the diagnostic isn't spammy.
        static std::uint32_t s_last_warned_id = 0;
        if (h.id != s_last_warned_id) {
            std::fprintf(stderr,
                "[psy::gpu::mtl] bind_pipeline: PipelineHandle id=%u not registered "
                "(lane 08 owes psynder_gx_mtl_register_render_pso); pass will render blank\n",
                h.id);
            s_last_warned_id = h.id;
        }
        c->bound_render_pso  = nil;
        c->bound_compute_pso = nil;
        return;
    }

    // entry->render_pso / compute_pso are stored as untyped `id` (the
    // shim's struct must not name protocols inside the nested namespace —
    // see the comment in pipeline_shim). Re-narrow back to the typed
    // protocol via static_cast on the way out.
    id<MTLRenderPipelineState>  render_pso  = static_cast<id<MTLRenderPipelineState>>(entry->render_pso);
    id<MTLComputePipelineState> compute_pso = static_cast<id<MTLComputePipelineState>>(entry->compute_pso);

    if (c->encoder && render_pso) {
        c->bound_render_pso = render_pso;
        [c->encoder setRenderPipelineState:render_pso];
    }
    if (c->compute_encoder && compute_pso) {
        c->bound_compute_pso = compute_pso;
        c->compute_tgs       = entry->compute_tgs;
        [c->compute_encoder setComputePipelineState:compute_pso];
    }
    // If neither encoder is open yet, stash for the next encode-open.
    if (!c->encoder && !c->compute_encoder) {
        c->bound_render_pso  = render_pso;
        c->bound_compute_pso = compute_pso;
        c->compute_tgs       = entry->compute_tgs;
    }
}

void MetalBackend::bind_vertex_buffer(CmdBuffer* cmd, std::uint32_t binding,
                                      Buffer* buf, std::uint64_t offset) {
    auto* c = static_cast<MtlCmdBuf*>(cmd);
    if (!c || !c->encoder) return;
    if (!buf) return;
    auto* mb = static_cast<MtlBuffer*>(buf);
    if (!mb->handle) return;
    [c->encoder setVertexBuffer:mb->handle
                          offset:(NSUInteger)offset
                         atIndex:(kVertexBufferSlot0 + binding)];
}

void MetalBackend::bind_index_buffer(CmdBuffer* cmd, Buffer* buf,
                                     IndexType type, std::uint64_t offset) {
    auto* c = static_cast<MtlCmdBuf*>(cmd);
    if (!c) return;
    if (!buf) {
        c->index_mtl        = nil;
        c->index_mtl_offset = 0;
        return;
    }
    auto* mb = static_cast<MtlBuffer*>(buf);
    c->index_mtl        = mb->handle;
    c->index_mtl_type   = (type == IndexType::U32) ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
    c->index_mtl_offset = offset;
}

void MetalBackend::push_constants(CmdBuffer* cmd, const void* data,
                                  std::uint32_t size, std::uint32_t stage_mask) {
    auto* c = static_cast<MtlCmdBuf*>(cmd);
    if (!c || !data || size == 0) return;

    const NSUInteger len = (NSUInteger)size;
    if (c->encoder) {
        if (stage_mask & 1u /*Vertex*/) {
            [c->encoder setVertexBytes:data length:len atIndex:kPushConstantSlot];
        }
        if (stage_mask & 2u /*Fragment*/) {
            [c->encoder setFragmentBytes:data length:len atIndex:kPushConstantSlot];
        }
    }
    if (c->compute_encoder) {
        if (stage_mask & 4u /*Compute*/) {
            [c->compute_encoder setBytes:data length:len atIndex:kPushConstantSlot];
        }
    }
    // Mesh-shader stage (bit 8) routes through setMeshBytes on macOS 13+;
    // not used at M1/M2 — left as a future extension when lane 09 ships
    // its mesh-shader path.
}

void MetalBackend::draw(CmdBuffer* cmd, std::uint32_t vc, std::uint32_t ic,
                        std::uint32_t fv, std::uint32_t fi) {
    auto* c = static_cast<MtlCmdBuf*>(cmd);
    if (!c || !c->encoder) return;
    if (!c->bound_render_pso) return; // no PSO = no work
    [c->encoder drawPrimitives:MTLPrimitiveTypeTriangle
                  vertexStart:(NSUInteger)fv
                  vertexCount:(NSUInteger)vc
                 instanceCount:(NSUInteger)(ic ? ic : 1)
                  baseInstance:(NSUInteger)fi];
}

void MetalBackend::draw_indexed(CmdBuffer* cmd, std::uint32_t ic, std::uint32_t inst,
                                std::uint32_t fi, std::int32_t vo, std::uint32_t fii) {
    auto* c = static_cast<MtlCmdBuf*>(cmd);
    if (!c || !c->encoder) return;
    if (!c->bound_render_pso) return;
    if (!c->index_mtl)      return;

    // first_index is an offset into the index buffer measured in indices;
    // Metal expects bytes, so multiply by the element size.
    const NSUInteger idx_stride =
        (c->index_mtl_type == MTLIndexTypeUInt32) ? 4u : 2u;
    const NSUInteger first_idx_byte_offset =
        (NSUInteger)c->index_mtl_offset + (NSUInteger)fi * idx_stride;

    [c->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:(NSUInteger)ic
                             indexType:c->index_mtl_type
                           indexBuffer:c->index_mtl
                     indexBufferOffset:first_idx_byte_offset
                         instanceCount:(NSUInteger)(inst ? inst : 1)
                            baseVertex:(NSInteger)vo
                          baseInstance:(NSUInteger)fii];
}

void MetalBackend::dispatch(CmdBuffer* cmd, std::uint32_t gx,
                            std::uint32_t gy, std::uint32_t gz) {
    @autoreleasepool {
        auto* c = static_cast<MtlCmdBuf*>(cmd);
        if (!c || !c->cb) return;
        // Compute encoders are scoped: open one lazily, dispatch, close.
        if (c->encoder) {
            // A render encoder is live — Metal can't co-exist with compute
            // in the same scope. Render lanes must end_render() first.
            std::fputs("[psy::gpu::mtl] dispatch called inside a render pass; ignored\n", stderr);
            return;
        }
        if (!c->compute_encoder) {
            c->compute_encoder = [c->cb computeCommandEncoder];
            c->compute_encoder.label = @"psy::gpu user compute";
            if (c->bound_compute_pso) {
                [c->compute_encoder setComputePipelineState:c->bound_compute_pso];
            }
        }
        if (!c->bound_compute_pso) {
            // No PSO bound — close the encoder cleanly and bail. Logged
            // once because lane 08 hasn't shipped Metal compute pipelines
            // yet (the same handshake limitation as render PSOs).
            [c->compute_encoder endEncoding];
            c->compute_encoder = nil;
            return;
        }
        MTLSize grid = MTLSizeMake(gx ? gx : 1, gy ? gy : 1, gz ? gz : 1);
        [c->compute_encoder dispatchThreadgroups:grid
                            threadsPerThreadgroup:c->compute_tgs];
        // M1: close the compute encoder after every dispatch so a
        // subsequent begin_render in the same cmd buffer works. M2 work
        // (multiple back-to-back dispatches) will batch this without
        // re-opening per dispatch.
        [c->compute_encoder endEncoding];
        c->compute_encoder = nil;
        frame_.encoded_clear = true; // user took the frame; skip auto-clear
    }
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
