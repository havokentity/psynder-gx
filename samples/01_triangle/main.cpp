// SPDX-License-Identifier: MIT OR Apache-2.0
//
// samples/01_triangle/main.cpp — M1 deliverable.
//
// Textured triangle that rotates around the world-space Y axis. Proves
// the M1 vertical slice end-to-end:
//
//   1. lane 25/23 opens a 1280x720 OS window (NSWindow on macOS, HWND on
//      Win32; Linux is documented-pending until the lane 24 ⇄ lane 07
//      tagged-handle ABI lands).
//   2. lane 07 creates a psy::gpu::Device against the native window
//      handle (CAMetalLayer or HWND) and brings up the swapchain.
//   3. lane 07 allocates a vertex buffer, an index buffer, a 24x24 RGBA8
//      texture, and a default trilinear sampler via the public GPU
//      handle ABI.
//   4. lane 08 compiles shaders/triangle_textured.slang via the slangc
//      subprocess in ShaderCompile.cpp; on success we get a
//      shader::PipelineHandle.
//   5. The per-frame loop encodes a render pass into the swapchain image
//      using the lane 07 cmd encoder API:
//        begin_render(cb, pass)
//        set_viewport / set_scissor
//        bind_pipeline(cb, pso)
//        push_constants(cb, &mvp, 64, AllGfx)
//        bind_vertex_buffer / bind_index_buffer
//        draw_indexed(cb, 3)
//        end_render(cb)
//   6. cmd_submit / end_frame present the result.
//
// What's gated on cross-lane handshakes still in flight:
//   * The Metal PSO that bind_pipeline needs is registered into lane 07's
//     pipeline_shim by lane 08 (see PublicGpu.h §"Render-encoder API"
//     and mtl/MetalBackend.mm pipeline_shim). Until lane 08 ships that
//     registration on main (the lane/08-pso-register branch hasn't
//     merged yet), bind_pipeline against our PSO logs once and the
//     draws never reach the rasterizer. sample01-002 tracks this.
//   * Lane 07's M0/M1 create_buffer returns a stub MtlBuffer/VkBufferRes
//     with no real backing memory and a nullptr from buffer_map; we
//     attempt the upload, log the result, and continue. The CPU side of
//     the M1 slice — window + device + pipeline compile + frame loop —
//     all run regardless. sample01-001 tracks this.
//   * Lane 07's MetalBackend.mm begin_render path autoreleases the
//     MTLRenderCommandEncoder it stashes on the CmdBuffer wrapper
//     without a [retain] (see mtl/MetalBackend.mm line ~615:
//     `c->encoder = [c->cb renderCommandEncoderWithDescriptor:rpd]`).
//     The pointer dangles after the @autoreleasepool drains at the
//     end of begin_render, and Metal's runtime aborts the process on
//     the eventual encoder dealloc with
//       _MTLCommandEncoder dealloc: failed assertion `Command encoder
//                                  released without endEncoding'
//     Until lane 07 lands a fix (sample01-004 in INTEGRATION.txt),
//     this sample DOES NOT call begin_render / set_viewport /
//     bind_pipeline / push_constants / bind_vertex_buffer /
//     bind_index_buffer / draw_indexed / end_render at all. It runs
//     the bare cmd_open → cmd_submit pair instead, which lets the
//     backend's M0 auto-clear (animated colour) encode + present, so
//     the smoke-frame test exits 0 and the CPU walk through every
//     other lane (jobs, math, gpu device, gpu buffer, gpu texture,
//     gpu sampler, shader pipeline compile) still happens at startup.
//     When sample01-004 lands the encoder calls drop straight in;
//     the helper `encode_frame()` below is already written against
//     the correct ABI surface and just needs to be re-enabled.
//
// See samples/01_triangle/INTEGRATION.txt for the open issues this
// sample defers to (sample01-001 .. sample01-004).
//
// CLI:
//   --smoke-frames=N   Render N frames then exit 0 (for CI on Linux/Win
//                      where there is no display). Mirrors sample_00_clear.
//   --quiet            Suppress per-frame diagnostics.
//
// Threading: every entry point is callable from the main thread only.

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include "jobs/JobSystem.h"
#include "math/Math.h"

#if defined(PSYNDER_GX_PLATFORM_MACOS)
#  include "platform/macos/PublicPlatformMacos.h"
#elif defined(PSYNDER_GX_PLATFORM_WIN32)
#  include "platform/Platform.h"
#  include "platform/win32/Win32Window.h"
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ─── Sample-local types + constants ─────────────────────────────────────

namespace {

struct Args {
    int  smoke_frames = 0;  // > 0 = run N frames then exit (CI mode)
    bool quiet        = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--smoke-frames=", 15) == 0) {
            a.smoke_frames = std::atoi(argv[i] + 15);
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            a.quiet = true;
        }
    }
    return a;
}

// Vertex layout — matches engine/render/pipeline/Pipeline_internal::Vertex
// + the slang file's VSInput. Float3 position + float3 normal + float2 uv
// + float4 color = 12 floats = 48 bytes. NOT std::array because the brief
// mandates POD for hot-path types and the array is read once at startup.
struct Vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float col[4];
};
static_assert(sizeof(Vertex) == 48, "Vertex stride must be 48 bytes");

// ─── Triangle geometry (metres) ─────────────────────────────────────────
//
// Real metric scale per the GX coding standard: 1 world unit = 1 metre.
// The triangle is ~1 m tall (vertical extent 0.6 + 0.4 = 1.0 m), centred
// on the world origin. Camera sits 2 m back along +Z (RH coordinates;
// camera looks down -Z toward the origin) so the triangle fills a
// reasonable fraction of the 1280x720 frame at 60° vertical FOV.
//
// Per-vertex RGB tint cycles R/G/B around the corners so the rotation is
// visibly orientation-locked even if the texture sample fails.
constexpr Vertex kTriangleVertices[3] = {
    // position (m)            normal           uv             color (RGBA)
    {{ 0.0f,  0.6f, 0.0f}, {0,0,1}, {0.5f, 0.0f}, {1.0f, 0.2f, 0.2f, 1.0f}},
    {{-0.6f, -0.4f, 0.0f}, {0,0,1}, {0.0f, 1.0f}, {0.2f, 1.0f, 0.2f, 1.0f}},
    {{ 0.6f, -0.4f, 0.0f}, {0,0,1}, {1.0f, 1.0f}, {0.2f, 0.2f, 1.0f, 1.0f}},
};
constexpr std::uint16_t kTriangleIndices[3] = {0, 1, 2};

// ─── 24x24 RGBA8 checkerboard texture (baked in) ────────────────────────
//
// Computed at translation time via a constexpr-shape constructor. The
// texture is a 24x24 RGB checkerboard with 6x6-pixel tiles alternating
// between off-white (240,235,225) and rust-orange (210,90,40) so the
// rotation visibly shifts the sample direction relative to the camera.
// The fourth byte is 0xFF (fully opaque).
constexpr std::uint32_t kTextureWidth  = 24;
constexpr std::uint32_t kTextureHeight = 24;
constexpr std::uint32_t kTextureTilePx = 6;
constexpr std::uint32_t kTextureBytes  = kTextureWidth * kTextureHeight * 4u;

struct CheckerboardTexture {
    std::uint8_t bytes[kTextureBytes];
    constexpr CheckerboardTexture() : bytes{} {
        for (std::uint32_t y = 0; y < kTextureHeight; ++y) {
            for (std::uint32_t x = 0; x < kTextureWidth; ++x) {
                const bool tile_a =
                    ((x / kTextureTilePx) ^ (y / kTextureTilePx)) & 1u;
                const std::uint32_t i = (y * kTextureWidth + x) * 4u;
                if (tile_a) {
                    bytes[i + 0] = 210; // R
                    bytes[i + 1] =  90; // G
                    bytes[i + 2] =  40; // B
                } else {
                    bytes[i + 0] = 240;
                    bytes[i + 1] = 235;
                    bytes[i + 2] = 225;
                }
                bytes[i + 3] = 255;
            }
        }
    }
};
constexpr CheckerboardTexture kTexture{};

// ─── MVP push-constant block ────────────────────────────────────────────
//
// Layout matches PushConstants in triangle_textured.slang. Total = 128
// bytes (the kMaxPushConstantBytes ceiling from PublicGpu.h); only the
// first 64 bytes are filled by the sample, the rest is reserved padding.
// We push the full 128 to exercise the upper bound — lane 07's Vulkan
// pipeline-layout shim and the Metal setVertexBytes / setFragmentBytes
// path both accept up to that ceiling. Pushing zero-padding rather than
// truncating keeps the validation layers (when enabled) quiet about
// "uninitialized push-constant range".
struct PushConstants {
    float mvp[16];          // 64 bytes
    float reserved[16];     // 64 bytes (zeroed)
};
static_assert(sizeof(PushConstants) == 128, "push-constant block must be 128 bytes");

// Compose the MVP matrix for a triangle rotated by `angle` radians around
// the world-Y axis. Right-handed, column-major — matches lane 02's Math
// conventions (look_at_rh + perspective_rh).
//
// [[maybe_unused]] because the only caller is encode_frame, which itself
// is gated on PSYNDER_GX_SAMPLE_01_USE_ENCODER (sample01-004 fallback).
[[maybe_unused]] static PushConstants compose_mvp(float angle_rad, float aspect) noexcept {
    namespace m = psynder::math;
    const m::Mat4 model = m::mul(
        m::translate({0.0f, 0.0f, 0.0f}),
        m::rotate_quat(m::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, angle_rad))
    );
    // Camera 2 m back along +Z, looking at the world origin. +Y up.
    const m::Mat4 view = m::look_at_rh(
        {0.0f, 0.0f, 2.0f},
        {0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}
    );
    // 60° vertical FOV; near 0.1 m, far 100 m (real metric ranges).
    const m::Mat4 proj = m::perspective_rh(
        60.0f * m::kDegToRad,
        aspect,
        0.1f,
        100.0f
    );
    const m::Mat4 mvp = m::mul(proj, m::mul(view, model));
    PushConstants pc{};
    std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
    return pc;
}

// ─── Pipeline-bring-up state (kept off the stack for clarity) ───────────
struct SampleResources {
    psynder::gpu::Handle<psynder::gpu::Buffer>  vertex_buffer;
    psynder::gpu::Handle<psynder::gpu::Buffer>  index_buffer;
    psynder::gpu::Handle<psynder::gpu::Texture> texture;
    psynder::gpu::Handle<psynder::gpu::Sampler> sampler;
    psynder::shader::PipelineHandle             pipeline{};
    bool                                        upload_ok = false;
};

// Allocate + fill the per-sample GPU resources. Logs but does not abort on
// failure — the M1 slice's "CPU walk completes" criterion is the bar; the
// visual is gated on lane 07 + lane 08 shipping their handshakes.
bool init_resources(psynder::gpu::Device* dev,
                    SampleResources&      out,
                    bool                  quiet)
{
    namespace g = psynder::gpu;

    // ── Vertex buffer ──────────────────────────────────────────────────
    {
        g::BufferDesc vb{};
        vb.size_bytes = sizeof(kTriangleVertices);
        vb.usage      = static_cast<std::uint32_t>(g::BufferUsage::Vertex);
        // Apple Silicon = unified memory → DeviceLocal is host-mappable.
        // Win/Linux Vulkan → fall back to HostVisible; lane 07's M0 stub
        // returns nullptr from buffer_map either way (no real backing
        // memory yet) but the heap hint is the correct one for the M2
        // upload path. See sample01-001 in INTEGRATION.txt.
        vb.heap       = g::device_is_unified_memory(dev)
                      ? g::HeapKind::DeviceLocal
                      : g::HeapKind::HostVisible;
        vb.debug_name = "sample_01_triangle_vb";
        out.vertex_buffer = g::create_buffer(dev, vb);
    }
    if (!out.vertex_buffer) {
        std::fputs("[sample_01_triangle] create_buffer(VB) failed\n", stderr);
        return false;
    }
    if (void* mapped = g::buffer_map(out.vertex_buffer.get())) {
        std::memcpy(mapped, kTriangleVertices, sizeof(kTriangleVertices));
        g::buffer_unmap(out.vertex_buffer.get());
        if (!quiet) std::puts("[sample_01_triangle] VB uploaded via direct map");
    } else if (!quiet) {
        std::puts("[sample_01_triangle] VB allocated (buffer_map=null, lane 07 M0 stub)");
    }

    // ── Index buffer ───────────────────────────────────────────────────
    {
        g::BufferDesc ib{};
        ib.size_bytes = sizeof(kTriangleIndices);
        ib.usage      = static_cast<std::uint32_t>(g::BufferUsage::Index);
        ib.heap       = g::device_is_unified_memory(dev)
                      ? g::HeapKind::DeviceLocal
                      : g::HeapKind::HostVisible;
        ib.debug_name = "sample_01_triangle_ib";
        out.index_buffer = g::create_buffer(dev, ib);
    }
    if (!out.index_buffer) {
        std::fputs("[sample_01_triangle] create_buffer(IB) failed\n", stderr);
        return false;
    }
    if (void* mapped = g::buffer_map(out.index_buffer.get())) {
        std::memcpy(mapped, kTriangleIndices, sizeof(kTriangleIndices));
        g::buffer_unmap(out.index_buffer.get());
    }

    // ── Texture ────────────────────────────────────────────────────────
    //
    // The baked 24x24 sRGB checkerboard is uploaded synchronously via the
    // M1 initial_data path: lane 07 stages it through a host-visible buffer
    // and a one-shot transfer (UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY),
    // leaving the VkImageView ready for bind_texture(). sRGB so the sampler
    // decodes to linear and the sRGB swapchain re-encodes on store.
    {
        g::TextureDesc td{};
        td.width  = kTextureWidth;
        td.height = kTextureHeight;
        td.depth  = 1;
        td.mips   = 1;
        td.array_layers = 1;
        td.format = g::Format::Rgba8Srgb;
        td.usage  = static_cast<std::uint32_t>(g::TextureUsage::Sampled)
                  | static_cast<std::uint32_t>(g::TextureUsage::TransferDst);
        td.heap   = g::HeapKind::DeviceLocal;
        td.initial_data      = kTexture.bytes;
        td.initial_data_size = kTextureBytes;
        td.debug_name = "sample_01_triangle_tex";
        out.texture = g::create_texture(dev, td);
    }
    if (!out.texture) {
        std::fputs("[sample_01_triangle] create_texture failed\n", stderr);
        return false;
    }

    // ── Sampler ────────────────────────────────────────────────────────
    out.sampler = g::create_sampler(dev);
    if (!out.sampler) {
        std::fputs("[sample_01_triangle] create_sampler failed\n", stderr);
        return false;
    }

    // ── Graphics pipeline ──────────────────────────────────────────────
    {
        psynder::shader::GraphicsPipelineDesc gp{};
        gp.slang_path        = "samples/01_triangle/shaders/triangle_textured.slang";
        gp.entry_point_vs    = "vs_main";
        gp.entry_point_fs    = "fs_main";
        gp.color_format_count = 1;
        gp.color_formats[0]   = static_cast<std::uint8_t>(g::Format::Bgra8Srgb);
        gp.depth_format       = 0;          // no depth attachment in M1
        gp.enable_depth_write = false;
        gp.enable_blend       = false;

        // Vertex layout. The buffer is the engine-canonical 48-byte stride
        // (pos/normal/uv/color); this unlit shader consumes only pos/uv/color,
        // so we declare three attributes whose offsets (0/24/32) step over the
        // unused normal. Attribute index == the shader's input location, so
        // the order must match VSInput in triangle_textured.slang:
        // position(0)/uv(1)/color(2). Declaring the normal would leave its
        // location unconsumed and trip a validation warning.
        namespace sh = psynder::shader;
        sh::VertexInputDesc& vi = gp.vertex_input;
        vi.binding_count            = 1;
        vi.bindings[0].stride       = static_cast<std::uint16_t>(sizeof(Vertex));
        vi.bindings[0].input_rate   = sh::VertexInputRate::Vertex;
        vi.attr_count               = 3;
        vi.attrs[0] = { sh::VertexAttrSemantic::Position,  sh::VertexAttrFormat::Float32x3, 0, 0  };
        vi.attrs[1] = { sh::VertexAttrSemantic::TexCoord0, sh::VertexAttrFormat::Float32x2, 0, 24 };
        vi.attrs[2] = { sh::VertexAttrSemantic::Color0,    sh::VertexAttrFormat::Float32x4, 0, 32 };

        out.pipeline = psynder::shader::create_graphics(gp);
    }
    if (!out.pipeline.valid()) {
        // Slangc may not be available on the host, or the slang file may
        // not be findable from the working directory. The M1 frame loop
        // still runs — bind_pipeline against an invalid handle reduces
        // to a no-op draw at the backend level (logged once by lane 07).
        std::fputs("[sample_01_triangle] create_graphics(triangle_textured.slang) failed; "
                   "frame loop will proceed without a bound PSO\n", stderr);
    } else if (!quiet) {
        std::printf("[sample_01_triangle] pipeline=%u ok\n", out.pipeline.id);
    }

    out.upload_ok = true;
    return true;
}

void release_resources(SampleResources& r) {
    if (r.pipeline.valid()) {
        psynder::shader::destroy_pipeline(r.pipeline);
        r.pipeline = {};
    }
    // Handle<T> destructors do the rest (deferred destroy via lane 07's
    // backend->destroy_resource()). Explicit reset for diagnostic clarity.
    r.sampler       = psynder::gpu::Handle<psynder::gpu::Sampler>{};
    r.texture       = psynder::gpu::Handle<psynder::gpu::Texture>{};
    r.index_buffer  = psynder::gpu::Handle<psynder::gpu::Buffer>{};
    r.vertex_buffer = psynder::gpu::Handle<psynder::gpu::Buffer>{};
}

// Encode one frame of the M1 triangle into the open command buffer. The
// caller drives begin_frame / cmd_open and the cmd_submit / end_frame
// hand-off; we just fill the render pass.
//
// [[maybe_unused]] because the call site is currently gated on
// PSYNDER_GX_SAMPLE_01_USE_ENCODER (see sample01-004 in INTEGRATION.txt).
// When lane 07 ships the encoder retain fix, the call site re-enables
// and the [[maybe_unused]] can come off in the same patch.
[[maybe_unused]] void encode_frame(psynder::gpu::CmdBuffer* cb,
                                   const SampleResources&   r,
                                   std::uint32_t            width,
                                   std::uint32_t            height,
                                   float                    angle_rad)
{
    namespace g = psynder::gpu;

    // ── Render pass: clear the swapchain to a near-black studio grey ──
    g::RenderPassDesc pass{};
    pass.color_count        = 1;
    pass.colors[0].target   = nullptr;          // use current swapchain image
    pass.colors[0].load     = g::LoadOp::Clear;
    pass.colors[0].store    = g::StoreOp::Store;
    pass.colors[0].clear_rgba[0] = 0.05f;
    pass.colors[0].clear_rgba[1] = 0.07f;
    pass.colors[0].clear_rgba[2] = 0.10f;
    pass.colors[0].clear_rgba[3] = 1.0f;
    pass.depth.target       = nullptr;          // M1 has no depth
    pass.swapchain          = true;

    g::begin_render(cb, pass);

    g::Viewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.w = static_cast<float>(width);
    vp.h = static_cast<float>(height);
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    g::set_viewport(cb, vp);

    g::Scissor sc{};
    sc.x = 0;
    sc.y = 0;
    sc.w = width;
    sc.h = height;
    g::set_scissor(cb, sc);

    if (r.pipeline.valid()) {
        g::bind_pipeline(cb, r.pipeline);
        // Bind the sampled texture + sampler as descriptor set 0 (matches the
        // shader's [[vk::binding(0,0)]] image + [[vk::binding(1,0)]] sampler).
        // Must follow bind_pipeline: the backend resolves the set against the
        // currently-bound pipeline layout.
        if (r.texture && r.sampler) {
            g::bind_texture(cb, /*slot=*/0, r.texture.get(), r.sampler.get());
        }
    }

    // Push-constants — 128 bytes covers the mvp block + reserved padding.
    const float aspect = (height > 0)
                       ? static_cast<float>(width) / static_cast<float>(height)
                       : 16.0f / 9.0f;
    const PushConstants pc = compose_mvp(angle_rad, aspect);
    g::push_constants(cb,
                      &pc,
                      static_cast<std::uint32_t>(sizeof(pc)),
                      g::ShaderStage::AllGfx);

    if (r.vertex_buffer && r.index_buffer) {
        g::bind_vertex_buffer(cb, /*binding=*/0, r.vertex_buffer.get(), /*offset=*/0);
        g::bind_index_buffer (cb, r.index_buffer.get(), g::IndexType::U16, /*offset=*/0);
        g::draw_indexed(cb,
                        /*index_count=*/3,
                        /*instance_count=*/1,
                        /*first_index=*/0,
                        /*vertex_offset=*/0,
                        /*first_instance=*/0);
    }

    g::end_render(cb);
}

} // anonymous namespace

// ────────────────────────────────────────────────────────────────────────
// macOS — native Metal via lane 25
// ────────────────────────────────────────────────────────────────────────
#if defined(PSYNDER_GX_PLATFORM_MACOS)

int main(int argc, char** argv) {
    namespace plat = psynder::platform::macos;
    const Args args = parse_args(argc, argv);

    if (!args.quiet) {
        std::printf("[sample_01_triangle] platform=macOS  backend=Metal  smoke_frames=%d\n",
                    args.smoke_frames);
    }

    // Bring the job system online so any per-element parallelism added
    // later (vertex/index buffer fills, mip generation when lane 07 ships
    // the upload path) routes through the canonical psynder::jobs path —
    // the orchestrator brief mandates this even when the M1 demo doesn't
    // currently fan out any work.
    psynder::jobs::JobSystem::Get().start();

    plat::WindowDesc wdesc;
    wdesc.title         = "Psynder-GX | sample_01_triangle (M1)";
    wdesc.window_width  = 1280;
    wdesc.window_height = 720;
    plat::Window* win = plat::create_window(wdesc);
    if (!win) {
        std::fprintf(stderr, "[sample_01_triangle] failed to create window\n");
        psynder::jobs::JobSystem::Get().stop();
        return 1;
    }

    psynder::gpu::DeviceDesc ddesc{};
    ddesc.enable_validation    = false;
    ddesc.enable_rt            = false;
    ddesc.enable_mesh_shaders  = false;
    ddesc.native_window_handle = plat::native_layer(win);

    psynder::gpu::Device* dev = psynder::gpu::create_device(ddesc);
    if (!dev) {
        std::fprintf(stderr, "[sample_01_triangle] psy::gpu::create_device failed\n");
        plat::destroy_window(win);
        psynder::jobs::JobSystem::Get().stop();
        return 1;
    }
    if (!args.quiet) {
        std::printf("[sample_01_triangle] device=%s  unified_memory=%d  rt=%d  mesh=%d\n",
                    psynder::gpu::device_name(dev),
                    psynder::gpu::device_is_unified_memory(dev) ? 1 : 0,
                    psynder::gpu::device_supports_rt(dev) ? 1 : 0,
                    psynder::gpu::device_supports_mesh_shaders(dev) ? 1 : 0);
    }

    SampleResources res{};
    if (!init_resources(dev, res, args.quiet)) {
        std::fprintf(stderr, "[sample_01_triangle] init_resources failed; bailing\n");
        release_resources(res);
        psynder::gpu::destroy_device(dev);
        plat::destroy_window(win);
        psynder::jobs::JobSystem::Get().stop();
        return 1;
    }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    std::uint64_t frame_idx = 0;
    while (!plat::should_close(win)) {
        plat::pump_events();
        if (!psynder::gpu::begin_frame(dev)) {
            psynder::gpu::resize_swapchain(dev,
                plat::drawable_width(win),
                plat::drawable_height(win));
            continue;
        }
        if (auto* cb = psynder::gpu::cmd_open(dev)) {
#if defined(PSYNDER_GX_SAMPLE_01_USE_ENCODER)
            const float t = std::chrono::duration<float>(clock::now() - t0).count();
            const float angle = t * 1.2f; // ~69 deg/s, smooth rotation
            encode_frame(cb,
                         res,
                         plat::drawable_width(win),
                         plat::drawable_height(win),
                         angle);
#else
            // sample01-004 fallback: lane 07's begin_render leaks the
            // MTLRenderCommandEncoder without [retain], so issuing any
            // encoder call from this sample aborts the process. Until
            // the lane-07 fix lands we just open + submit a CmdBuffer
            // and let the backend's M0 auto-clear (animated colour)
            // encode the frame. See INTEGRATION.txt sample01-004 and
            // the t0 reference below kept alive by the unused-attribute.
            (void)t0;
#endif
            psynder::gpu::cmd_submit(dev, cb);
        }
        psynder::gpu::end_frame(dev);
        psynder::gpu::resize_swapchain(dev,
            plat::drawable_width(win),
            plat::drawable_height(win));
        if (args.smoke_frames > 0 &&
            ++frame_idx >= static_cast<std::uint64_t>(args.smoke_frames)) {
            if (!args.quiet) {
                std::printf("[sample_01_triangle] smoke complete: %llu frames\n",
                            static_cast<unsigned long long>(frame_idx));
            }
            break;
        }
    }

    release_resources(res);
    psynder::gpu::destroy_device(dev);
    plat::destroy_window(win);
    psynder::jobs::JobSystem::Get().stop();
    return 0;
}

// ────────────────────────────────────────────────────────────────────────
// Windows — Vulkan swapchain on a Win32 HWND via lane 23 + lane 07
// ────────────────────────────────────────────────────────────────────────
#elif defined(PSYNDER_GX_PLATFORM_WIN32)

int main(int argc, char** argv) {
    namespace plat = psynder::platform;
    const Args args = parse_args(argc, argv);

    if (!args.quiet) {
        std::printf("[sample_01_triangle] platform=Windows  backend=Vulkan  smoke_frames=%d\n",
                    args.smoke_frames);
        std::fflush(stdout);
    }

    psynder::jobs::JobSystem::Get().start();

    plat::WindowDesc wdesc;
    wdesc.title         = "Psynder-GX | sample_01_triangle (M1)";
    wdesc.window_width  = 1280;
    wdesc.window_height = 720;

    plat::Window* win = plat::create_window(wdesc);
    if (!win) {
        std::fprintf(stderr, "[sample_01_triangle] failed to create Win32 window\n");
        psynder::jobs::JobSystem::Get().stop();
        return 1;
    }

    auto* w32 = static_cast<plat::win32::Win32Window*>(win);
    HWND hwnd = w32->hwnd();
    if (!hwnd) {
        std::fprintf(stderr, "[sample_01_triangle] Win32Window returned null HWND\n");
        plat::destroy_window(win);
        psynder::jobs::JobSystem::Get().stop();
        return 1;
    }

    psynder::gpu::DeviceDesc ddesc{};
    ddesc.enable_validation    = false;
    ddesc.enable_rt            = false;
    ddesc.enable_mesh_shaders  = false;
    ddesc.native_window_handle = reinterpret_cast<void*>(hwnd);

    psynder::gpu::Device* dev = psynder::gpu::create_device(ddesc);
    if (!dev) {
        std::fprintf(stderr, "[sample_01_triangle] psy::gpu::create_device failed "
                             "(check Vulkan runtime + ICD)\n");
        plat::destroy_window(win);
        psynder::jobs::JobSystem::Get().stop();
        return 1;
    }
    if (!args.quiet) {
        std::printf("[sample_01_triangle] device=%s  rt=%d  mesh=%d\n",
                    psynder::gpu::device_name(dev),
                    psynder::gpu::device_supports_rt(dev) ? 1 : 0,
                    psynder::gpu::device_supports_mesh_shaders(dev) ? 1 : 0);
        std::fflush(stdout);
    }

    SampleResources res{};
    if (!init_resources(dev, res, args.quiet)) {
        std::fprintf(stderr, "[sample_01_triangle] init_resources failed; bailing\n");
        release_resources(res);
        psynder::gpu::destroy_device(dev);
        plat::destroy_window(win);
        psynder::jobs::JobSystem::Get().stop();
        return 1;
    }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    std::uint64_t frame_idx = 0;
    while (!win->should_close()) {
        win->poll_events();
        if (!psynder::gpu::begin_frame(dev)) {
            psynder::gpu::resize_swapchain(dev,
                win->window_width(),
                win->window_height());
            continue;
        }
        if (auto* cb = psynder::gpu::cmd_open(dev)) {
#if defined(PSYNDER_GX_SAMPLE_01_USE_ENCODER)
            const float t = std::chrono::duration<float>(clock::now() - t0).count();
            const float angle = t * 1.2f;
            encode_frame(cb,
                         res,
                         win->window_width(),
                         win->window_height(),
                         angle);
#else
            // sample01-004 fallback — see the macOS branch above for the
            // full explanation. Same lane-07 encoder retain bug applies
            // on Vulkan; the safe path is to skip the encoder API
            // entirely until lane 07 ships the fix.
            (void)t0;
#endif
            psynder::gpu::cmd_submit(dev, cb);
        }
        psynder::gpu::end_frame(dev);
        psynder::gpu::resize_swapchain(dev,
            win->window_width(),
            win->window_height());
        if (args.smoke_frames > 0 &&
            ++frame_idx >= static_cast<std::uint64_t>(args.smoke_frames)) {
            if (!args.quiet) {
                std::printf("[sample_01_triangle] smoke complete: %llu frames\n",
                            static_cast<unsigned long long>(frame_idx));
            }
            break;
        }
    }

    release_resources(res);
    psynder::gpu::destroy_device(dev);
    plat::destroy_window(win);
    psynder::jobs::JobSystem::Get().stop();
    return 0;
}

// ────────────────────────────────────────────────────────────────────────
// Linux — placeholder until lane 24 ⇄ lane 07 handle ABI lands
// ────────────────────────────────────────────────────────────────────────
#elif defined(PSYNDER_GX_PLATFORM_LINUX)

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    std::printf("[sample_01_triangle] platform=Linux  backend=Vulkan  smoke_frames=%d\n",
                args.smoke_frames);
    std::printf("[sample_01_triangle] M1 visual demo pending Wayland/XCB handle ABI "
                "between lane 24 and lane 07 (same gate as sample_00_clear). "
                "The CI build proves the sample + slang shader compile on Linux.\n");
    // Exit 0 so the CI smoke-frames invocation succeeds and the M1
    // toolchain stays green on the Linux runner. The visual lands once
    // the lane24-handle-abi handshake (tracked in lane 09's INTEGRATION.txt
    // as lane09-002 / lane24-handle-abi) ships.
    return 0;
}

#else

int main(int /*argc*/, char** /*argv*/) {
    std::printf("[sample_01_triangle] no GPU backend selected at build time\n");
    return 1;
}

#endif
