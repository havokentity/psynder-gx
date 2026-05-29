// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/PipelineCreateMetal.mm
//
// Lane 08 — Metal pipeline-state-object construction + lane-07 registration.
//
// Guarded by PSYNDER_GX_BACKEND_METAL. On non-macOS builds this TU is
// excluded from the lane CMake target (the lane helper drops .mm off Apple).
//
// Flow:
//   1. After slangc emits a Metal IR / .metallib blob, lane 08 loads it
//      via [device newLibraryWithData:error:] and grabs the named
//      entry-point as id<MTLFunction>.
//   2. Builds an MTLRenderPipelineDescriptor or MTLComputePipelineDescriptor.
//   3. Compiles to MTLRenderPipelineState / MTLComputePipelineState.
//   4. Forwards the state object to lane 07 via
//      psynder_gx_mtl_register_render_pso / *_compute_pso.
//
// Device handle: we use MTLCreateSystemDefaultDevice() which on macOS
// returns the same MTLDevice lane 07 obtained — Apple guarantees a
// single system-default device on a given machine (Apple Silicon).
// No extern-C device push from lane 07 is required, simplifying the
// handshake compared to the Vulkan side.
//
// Vertex layout — see DefaultVertexLayout in PipelineCreateBackend.h.
// M1 hard-codes interleaved float3 pos + float3 normal + float2 uv at
// binding 0. Metal slot used for vertex buffer is kVertexBufferSlot0 = 1
// (slot 0 is reserved for push-constants — see lane 07's MetalBackend.mm
// comment near kPushConstantSlot).

#include "shader/impl/PipelineCreateBackend.h"

#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#if defined(PSYNDER_GX_BACKEND_METAL)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

// ─── Lane-07 registration shims (extern "C" symbols defined in
//     engine/gpu/mtl/MetalBackend.mm) ───────────────────────────────────
extern "C" void psynder_gx_mtl_register_render_pso(
    std::uint32_t handle_id, void* mtl_pso, bool fill_wireframe);

extern "C" void psynder_gx_mtl_register_compute_pso(
    std::uint32_t handle_id, void* mtl_pso,
    std::uint32_t tgs_x, std::uint32_t tgs_y, std::uint32_t tgs_z);

namespace psynder::shader::impl {

namespace {

// Single-process MTLDevice cache. MTLCreateSystemDefaultDevice can be
// called multiple times — Apple returns the same underlying device on
// Apple Silicon — but it's still good practice to hold one reference.
//
// THREADING: create_graphics / create_compute may be called concurrently
// from lane 04's worker threads (per the cross-lane handshake documented
// at the top of ShaderCompile.cpp), so the device init and the alive-pso
// vectors must be guarded. We use std::once_flag for the device init
// (idempotent + zero-cost on the fast path) and std::mutex for the
// alive-vector appends + the pipeline-shim writes downstream.
id<MTLDevice>      g_mtl_device  = nil;
std::once_flag     g_init_once;

// Stable storage for shipped MTLLibrary / MTLFunction / MTLPipelineState
// objects. ARC is OFF for this TU (per CMakeLists -fno-objc-arc), so we
// hold owning references via `retain` and never release until process exit.
//
// We store the strong refs in a flat vector to keep them alive past the
// scope of the build function — the lane 07 shim only stores a raw `id`
// pointer (its CMake sets -fno-objc-arc on its own .mm too).
std::vector<id> g_alive_render_psos;
std::vector<id> g_alive_compute_psos;
std::vector<id> g_alive_libs;
std::mutex      g_alive_mu;   // guards the three vectors above

bool ensure_device() {
    std::call_once(g_init_once, []{
        g_mtl_device = MTLCreateSystemDefaultDevice();
        if (!g_mtl_device) {
            std::fputs("[psy::shader::mtl] MTLCreateSystemDefaultDevice returned nil; "
                       "lane 08 cannot build Metal PSOs (lane 07 will log "
                       "'PipelineHandle not registered' at bind time)\n", stderr);
        }
    });
    return g_mtl_device != nil;
}

// Build an MTLLibrary from a metallib blob.
// `metal_ir` is the .metallib bytes produced by `xcrun metallib`.
id<MTLLibrary> make_library(const std::vector<std::uint8_t>& metal_ir) {
    if (metal_ir.empty()) return nil;
    @autoreleasepool {
        dispatch_data_t data = dispatch_data_create(
            metal_ir.data(),
            metal_ir.size(),
            dispatch_get_main_queue(),  // queue is irrelevant here; data is copied
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (!data) return nil;

        NSError* err = nil;
        id<MTLLibrary> lib = [g_mtl_device newLibraryWithData:data error:&err];
        // dispatch_data_create incremented its own retain; release it.
        // (ARC is off — we own the +1 from dispatch_data_create.)
        // dispatch_data_t under MRC: dispatch_release is the matching op.
        dispatch_release(data);
        if (!lib) {
            std::fprintf(stderr,
                "[psy::shader::mtl] newLibraryWithData failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "(no error info)");
            return nil;
        }
        // Library is autoreleased; retain to keep alive past pool drain.
        return [lib retain];
    }
}

// ─── Public-VertexAttrFormat → MTLVertexFormat ────────────────────────────
MTLVertexFormat to_mtl_format(VertexAttrFormat f) {
    switch (f) {
        case VertexAttrFormat::Float32:     return MTLVertexFormatFloat;
        case VertexAttrFormat::Float32x2:   return MTLVertexFormatFloat2;
        case VertexAttrFormat::Float32x3:   return MTLVertexFormatFloat3;
        case VertexAttrFormat::Float32x4:   return MTLVertexFormatFloat4;
        case VertexAttrFormat::Uint8x4Norm: return MTLVertexFormatUChar4Normalized;
        case VertexAttrFormat::Uint16x2:    return MTLVertexFormatUShort2;
    }
    return MTLVertexFormatInvalid;
}

MTLVertexStepFunction to_mtl_step(VertexInputRate r) {
    return (r == VertexInputRate::Instance)
        ? MTLVertexStepFunctionPerInstance
        : MTLVertexStepFunctionPerVertex;
}

MTLPixelFormat to_mtl_depth_format(std::uint8_t format) {
    // Mirrors gpu::Format without depending on lane 07 headers here.
    constexpr std::uint8_t kDepth32Float = 8;
    constexpr std::uint8_t kDepth24UnormStencil8 = 9;
    switch (format) {
        case 0: return MTLPixelFormatInvalid;
        case kDepth32Float: return MTLPixelFormatDepth32Float;
        case kDepth24UnormStencil8: return MTLPixelFormatDepth24Unorm_Stencil8;
    }
    return MTLPixelFormatInvalid;
}

// Lane 07's MetalBackend.mm reserves Metal vertex-buffer slot 0 for
// push-constants. Both the default and the user-supplied paths offset
// caller slots by +1 so they sit at slot 1 (kVertexBufferSlot0) and up.
constexpr std::uint32_t kMtlPushConstantReserved = 1;

// Build an interleaved float3 pos + float3 normal + float2 uv vertex
// descriptor matching DefaultVertexLayout. Buffer slot is
// kMtlPushConstantReserved (=1) because slot 0 is reserved for
// push-constants per MetalBackend.mm.
MTLVertexDescriptor* make_default_vertex_descriptor() {
    MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];

    // attribute 0: position (float3) @ offset 0
    vd.attributes[0].format      = MTLVertexFormatFloat3;
    vd.attributes[0].offset      = DefaultVertexLayout::kAttribOffsets[0];
    vd.attributes[0].bufferIndex = kMtlPushConstantReserved;

    // attribute 1: normal (float3) @ offset 12
    vd.attributes[1].format      = MTLVertexFormatFloat3;
    vd.attributes[1].offset      = DefaultVertexLayout::kAttribOffsets[1];
    vd.attributes[1].bufferIndex = kMtlPushConstantReserved;

    // attribute 2: uv (float2) @ offset 24
    vd.attributes[2].format      = MTLVertexFormatFloat2;
    vd.attributes[2].offset      = DefaultVertexLayout::kAttribOffsets[2];
    vd.attributes[2].bufferIndex = kMtlPushConstantReserved;

    vd.layouts[kMtlPushConstantReserved].stride       = DefaultVertexLayout::kStrideBytes;
    vd.layouts[kMtlPushConstantReserved].stepRate     = 1;
    vd.layouts[kMtlPushConstantReserved].stepFunction = MTLVertexStepFunctionPerVertex;

    return vd; // caller takes ownership
}

// Build a vertex descriptor from a caller-supplied VertexInputDesc.
// Returns nil if the desc fails validate() (caller-controlled fatality
// surfaces a stderr line + a graceful PSO-create failure upstream).
//
// Caller's buffer_slot is offset by kMtlPushConstantReserved so the
// pushed-constant slot at index 0 stays untouched.  The caller drives
// the layout in "their" slot numbering — 0,1,2… — so this offset is
// transparent at the public surface.
MTLVertexDescriptor* make_vertex_descriptor_from(const VertexInputDesc& vi) {
    if (!validate(vi)) return nil;

    MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];

    for (std::uint8_t a = 0; a < vi.attr_count; ++a) {
        const VertexAttr& A = vi.attrs[a];
        vd.attributes[a].format      = to_mtl_format(A.format);
        vd.attributes[a].offset      = A.offset_in_buffer;
        vd.attributes[a].bufferIndex = A.buffer_slot + kMtlPushConstantReserved;
    }

    for (std::uint8_t b = 0; b < vi.binding_count; ++b) {
        const VertexBufferBinding& B = vi.bindings[b];
        const std::uint32_t mtl_slot = b + kMtlPushConstantReserved;
        // Resolve the implicit (0) stride to the tightly-packed stride; a 0
        // layout stride is invalid for an attributed Metal buffer layout.
        vd.layouts[mtl_slot].stride       = effective_binding_stride(vi, b);
        vd.layouts[mtl_slot].stepRate     = 1;
        vd.layouts[mtl_slot].stepFunction = to_mtl_step(B.input_rate);
    }

    return vd; // caller takes ownership
}

} // anonymous namespace

bool create_and_register_graphics_pso(
    std::uint32_t                     handle_id,
    const std::vector<std::uint8_t>&  vs_blob,
    const std::vector<std::uint8_t>&  fs_blob,
    const char*                       vs_entry,
    const char*                       fs_entry,
    const VertexInputDesc&            vertex_input,
    std::uint32_t                     color_format_count,
    std::uint8_t                      depth_format,
    bool                              enable_depth_write,
    bool                              enable_blend,
    bool                              fill_wireframe)
{
    (void)enable_depth_write; // Metal compare/write state is encoder-side.
    if (!ensure_device()) return false;
    if (vs_blob.empty() || fs_blob.empty()) return false;

    @autoreleasepool {
        // slangc emits a separate .metallib per entry-point in lane 08's
        // existing compile path (one invocation per entry). So vs_blob and
        // fs_blob are two distinct metallib blobs; we load each into its
        // own MTLLibrary and pull the entry point from the matching one.
        id<MTLLibrary> vs_lib = make_library(vs_blob);
        if (!vs_lib) return false;
        id<MTLLibrary> fs_lib = make_library(fs_blob);
        if (!fs_lib) { [vs_lib release]; return false; }

        NSString* vs_name = [NSString stringWithUTF8String:(vs_entry ? vs_entry : "vs_main")];
        NSString* fs_name = [NSString stringWithUTF8String:(fs_entry ? fs_entry : "fs_main")];
        id<MTLFunction> vs_fn = [vs_lib newFunctionWithName:vs_name];
        id<MTLFunction> fs_fn = [fs_lib newFunctionWithName:fs_name];
        if (!vs_fn || !fs_fn) {
            std::fprintf(stderr,
                "[psy::shader::mtl] newFunctionWithName failed: vs='%s' fs='%s'\n",
                vs_entry ? vs_entry : "vs_main",
                fs_entry ? fs_entry : "fs_main");
            if (vs_fn) [vs_fn release];
            if (fs_fn) [fs_fn release];
            [vs_lib release];
            [fs_lib release];
            return false;
        }

        MTLRenderPipelineDescriptor* rpd = [[MTLRenderPipelineDescriptor alloc] init];
        rpd.vertexFunction   = vs_fn;
        rpd.fragmentFunction = fs_fn;

        if (color_format_count != 0) {
            // Color attachment 0 — must match lane 07's CAMetalLayer.pixelFormat
            // verbatim or Metal validation triggers and rendering blanks. Lane 07
            // configures the layer as MTLPixelFormatBGRA8Unorm — NOT _sRGB.
            rpd.colorAttachments[0].pixelFormat     = MTLPixelFormatBGRA8Unorm;
            rpd.colorAttachments[0].blendingEnabled = enable_blend ? YES : NO;
            if (enable_blend) {
                rpd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
                rpd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
                rpd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
                rpd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
                rpd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                rpd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            }
        }

        const MTLPixelFormat mtl_depth_format = to_mtl_depth_format(depth_format);
        if (depth_format != 0 && mtl_depth_format == MTLPixelFormatInvalid) {
            std::fprintf(stderr,
                "[psy::shader::mtl] unsupported depth_format=%u for id=%u\n",
                static_cast<unsigned>(depth_format), handle_id);
            [rpd release];
            if (vs_fn) [vs_fn release];
            if (fs_fn) [fs_fn release];
            [vs_lib release];
            [fs_lib release];
            return false;
        }
        rpd.depthAttachmentPixelFormat = mtl_depth_format;
        rpd.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;

        // Vertex descriptor.
        //   * attr_count == 0  → DefaultVertexLayout (slot 1, stride 32).
        //   * attr_count  > 0  → translate the caller-supplied
        //                        VertexInputDesc, applying the lane-07
        //                        slot-0-reserved-for-push-constants offset.
        // On validate() failure, make_vertex_descriptor_from returns nil
        // and we log + bail with a clean PSO-create failure so lane 07
        // falls back to its "not registered" no-op draw path.
        MTLVertexDescriptor* vd = (vertex_input.attr_count == 0)
            ? make_default_vertex_descriptor()
            : make_vertex_descriptor_from(vertex_input);
        if (!vd) {
            std::fprintf(stderr,
                "[psy::shader::mtl] VertexInputDesc failed validate() "
                "(id=%u, attr_count=%u, binding_count=%u)\n",
                handle_id,
                (unsigned)vertex_input.attr_count,
                (unsigned)vertex_input.binding_count);
            [rpd release];
            [vs_fn release]; [fs_fn release];
            [vs_lib release]; [fs_lib release];
            return false;
        }
        rpd.vertexDescriptor = vd;

        NSError* err = nil;
        id<MTLRenderPipelineState> pso =
            [g_mtl_device newRenderPipelineStateWithDescriptor:rpd error:&err];

        [rpd release];
        [vd  release];
        [vs_fn release];
        [fs_fn release];

        if (!pso) {
            std::fprintf(stderr,
                "[psy::shader::mtl] newRenderPipelineStateWithDescriptor failed "
                "id=%u: %s\n",
                handle_id,
                err ? [[err localizedDescription] UTF8String] : "(no error info)");
            [vs_lib release];
            [fs_lib release];
            return false;
        }

        // Keep PSO + libs alive for the process lifetime (M1 leak; M2
        // integrates with lane 07's frames-in-flight reaper).  Guarded
        // because create_graphics can fire from multiple worker threads.
        {
            std::lock_guard<std::mutex> _(g_alive_mu);
            g_alive_render_psos.push_back(pso);
            g_alive_libs.push_back(vs_lib);
            g_alive_libs.push_back(fs_lib);
        }

        // Hand off to lane 07's registry. Untyped void* — the shim
        // bridge-casts to id<MTLRenderPipelineState> on its side
        // (also -fno-objc-arc).  The lane-07 shim is itself mutex-guarded
        // (see psynder_gx_mtl_register_render_pso in MetalBackend.mm).
        // Triangle fill mode is render-command-encoder state in Metal (NOT
        // part of the MTLRenderPipelineState), so the wireframe flag travels
        // alongside the PSO into lane 07's shim and is applied at bind time.
        psynder_gx_mtl_register_render_pso(handle_id, (__bridge void*)pso, fill_wireframe);
        return true;
    }
}

bool create_and_register_compute_pso(
    std::uint32_t                     handle_id,
    const std::vector<std::uint8_t>&  cs_blob,
    const char*                       cs_entry)
{
    if (!ensure_device()) return false;
    if (cs_blob.empty()) return false;

    @autoreleasepool {
        id<MTLLibrary> lib = make_library(cs_blob);
        if (!lib) return false;

        NSString* cs_name = [NSString stringWithUTF8String:(cs_entry ? cs_entry : "cs_main")];
        id<MTLFunction> cs_fn = [lib newFunctionWithName:cs_name];
        if (!cs_fn) {
            std::fprintf(stderr,
                "[psy::shader::mtl] newFunctionWithName(%s) failed for compute\n",
                cs_entry ? cs_entry : "cs_main");
            [lib release];
            return false;
        }

        NSError* err = nil;
        id<MTLComputePipelineState> pso =
            [g_mtl_device newComputePipelineStateWithFunction:cs_fn error:&err];

        if (!pso) {
            std::fprintf(stderr,
                "[psy::shader::mtl] newComputePipelineStateWithFunction failed "
                "id=%u: %s\n",
                handle_id,
                err ? [[err localizedDescription] UTF8String] : "(no error info)");
            [cs_fn release];
            [lib release];
            return false;
        }

        // Threadgroup size: M1 default 64,1,1 (a reasonable wave-aligned
        // default for Apple Silicon). Future shaders can request a
        // [numthreads(X,Y,Z)] tag via reflection — Slang exposes the
        // threadgroup size in its reflection JSON. M2 work to pipe that
        // through; for M1 we use the conservative default.
        // The actual encoded threadgroup-per-dispatch sizes come from
        // gpu::dispatch() arguments which are independent of this.
        const std::uint32_t tgs_x = 64, tgs_y = 1, tgs_z = 1;

        {
            std::lock_guard<std::mutex> _(g_alive_mu);
            g_alive_compute_psos.push_back(pso);
            g_alive_libs.push_back(lib);
        }
        [cs_fn release]; // pso retains its function internally

        psynder_gx_mtl_register_compute_pso(
            handle_id, (__bridge void*)pso, tgs_x, tgs_y, tgs_z);
        return true;
    }
}

void release_registered_pso(std::uint32_t /*handle_id*/) {
    // M1 leak-by-design — see header. M2 integrates with lane 07's reaper.
}

} // namespace psynder::shader::impl

#else  // !PSYNDER_GX_BACKEND_METAL

// On non-Metal builds this TU compiles empty. The Vulkan TU
// (PipelineCreateVulkan.cpp) supplies the symbol implementations.

#endif // PSYNDER_GX_BACKEND_METAL
