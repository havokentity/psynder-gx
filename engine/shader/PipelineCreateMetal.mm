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
#include <string>
#include <vector>

#if defined(PSYNDER_GX_BACKEND_METAL)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

// ─── Lane-07 registration shims (extern "C" symbols defined in
//     engine/gpu/mtl/MetalBackend.mm) ───────────────────────────────────
extern "C" void psynder_gx_mtl_register_render_pso(
    std::uint32_t handle_id, void* mtl_pso);

extern "C" void psynder_gx_mtl_register_compute_pso(
    std::uint32_t handle_id, void* mtl_pso,
    std::uint32_t tgs_x, std::uint32_t tgs_y, std::uint32_t tgs_z);

namespace psynder::shader::impl {

namespace {

// Single-process MTLDevice cache. MTLCreateSystemDefaultDevice can be
// called multiple times — Apple returns the same underlying device on
// Apple Silicon — but it's still good practice to hold one reference.
id<MTLDevice>      g_mtl_device  = nil;
bool               g_init_tried  = false;

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

bool ensure_device() {
    if (g_mtl_device) return true;
    if (g_init_tried) return g_mtl_device != nil;
    g_init_tried = true;
    g_mtl_device = MTLCreateSystemDefaultDevice();
    if (!g_mtl_device) {
        std::fputs("[psy::shader::mtl] MTLCreateSystemDefaultDevice returned nil; "
                   "lane 08 cannot build Metal PSOs (lane 07 will log "
                   "'PipelineHandle not registered' at bind time)\n", stderr);
    }
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

// Build an interleaved float3 pos + float3 normal + float2 uv vertex
// descriptor matching DefaultVertexLayout. Buffer slot is
// kVertexBufferSlot0 = 1 (slot 0 reserved for push-constants per
// MetalBackend.mm).
MTLVertexDescriptor* make_default_vertex_descriptor() {
    MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];

    // attribute 0: position (float3) @ offset 0
    vd.attributes[0].format      = MTLVertexFormatFloat3;
    vd.attributes[0].offset      = DefaultVertexLayout::kAttribOffsets[0];
    vd.attributes[0].bufferIndex = 1;  // kVertexBufferSlot0

    // attribute 1: normal (float3) @ offset 12
    vd.attributes[1].format      = MTLVertexFormatFloat3;
    vd.attributes[1].offset      = DefaultVertexLayout::kAttribOffsets[1];
    vd.attributes[1].bufferIndex = 1;

    // attribute 2: uv (float2) @ offset 24
    vd.attributes[2].format      = MTLVertexFormatFloat2;
    vd.attributes[2].offset      = DefaultVertexLayout::kAttribOffsets[2];
    vd.attributes[2].bufferIndex = 1;

    vd.layouts[1].stride       = DefaultVertexLayout::kStrideBytes;
    vd.layouts[1].stepRate     = 1;
    vd.layouts[1].stepFunction = MTLVertexStepFunctionPerVertex;

    return vd; // caller takes ownership
}

} // anonymous namespace

bool create_and_register_graphics_pso(
    std::uint32_t                     handle_id,
    const std::vector<std::uint8_t>&  vs_blob,
    const std::vector<std::uint8_t>&  fs_blob,
    const char*                       vs_entry,
    const char*                       fs_entry)
{
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

        // Color attachment 0 — match lane 07's swapchain pixel format.
        // CAMetalLayer.pixelFormat in MetalBackend defaults to
        // MTLPixelFormatBGRA8Unorm_sRGB; mirror that here.
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        rpd.colorAttachments[0].blendingEnabled = NO;

        // No depth attachment for M1 forward-to-swapchain.
        rpd.depthAttachmentPixelFormat   = MTLPixelFormatInvalid;
        rpd.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;

        // Vertex descriptor — interleaved float3+float3+float2 at slot 1.
        MTLVertexDescriptor* vd = make_default_vertex_descriptor();
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
        // integrates with lane 07's frames-in-flight reaper).
        g_alive_render_psos.push_back(pso);
        g_alive_libs.push_back(vs_lib);
        g_alive_libs.push_back(fs_lib);

        // Hand off to lane 07's registry. Untyped void* — the shim
        // bridge-casts to id<MTLRenderPipelineState> on its side
        // (also -fno-objc-arc).
        psynder_gx_mtl_register_render_pso(handle_id, (__bridge void*)pso);
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

        g_alive_compute_psos.push_back(pso);
        g_alive_libs.push_back(lib);
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
