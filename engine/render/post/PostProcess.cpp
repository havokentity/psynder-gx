// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/PostProcess.cpp
//
// Lane 11 - post-process stack: Karis bloom + ACES tonemap + UI composite.
//
// Frozen public API: psynder::render::post::{init, shutdown, run_frame,
//                    set_upscaler_runtime}. See PublicRenderPost.h.
//
// References:
//   - DESIGN-PSYNDER-GX.md section 7.8 (upscale / tonemap / UI composite).
//   - Brian Karis, "Tone Mapping" (SIGGRAPH 2013) -- 13-tap partial-Karis
//     downsample + 3x3 tent upsample.
//   - Krzysztof Narkowicz, "ACES Filmic Tone Mapping Curve" (2015).
//
// ─── Pipeline overview ─────────────────────────────────────────────────
//
//   Inputs:  scene_color  (linear HDR, full-res, set via set_scene_color)
//            ui_rml       (optional, sRGB, set via set_ui_textures)
//            ui_imm       (optional, sRGB, set via set_ui_textures)
//            backbuffer   (target, sRGB swapchain image)
//
//   Per frame:
//     1.  Bloom prefilter      scene  -> bloom_mip[1]      (threshold + Karis 13-tap downsample)
//     2.  Bloom downsample chain   bloom_mip[N]   -> bloom_mip[N+1]   for N=1..kBloomMips-2
//     3.  Bloom upsample chain     bloom_mip[N+1] -> bloom_mip[N]     for N=kBloomMips-2..1 (additive)
//     4.  Bloom composite      scene + bloom_mip[1] * strength -> hdr_bloom_out
//     5.  Tonemap              hdr_bloom_out -> tonemap_out            (ACES + exposure)
//     6.  UI composite         tonemap_out + ui_rml + ui_imm -> backbuffer
//
// ─── Resize limitation (M1 baseline) ───────────────────────────────────
//
// PublicRenderPost.h is FROZEN at Wave 0 and does not expose a
// `resize_output_size(...)` entry point. To change the post-chain
// resolution (e.g. window resize > new render-scale), the host MUST
// call shutdown() and then init() with the new PostDesc. The bloom
// pyramid and intermediate render targets are torn down and rebuilt.
//
// A non-blocking lane-11 follow-up issue is filed against the
// orchestrator to add `resize_output_size()` once the public-header
// freeze relaxes for Wave B.
//
// ─── Allocation discipline ─────────────────────────────────────────────
//
// All GPU resources (buffers, textures, pipelines) are allocated once in
// init() and released in shutdown(). run_frame() does NOT allocate
// per-frame -- the post stack is a hot path (16 ms budget at 60 Hz).
// Conformance: DESIGN section 4.4 + section 14 "no mid-frame allocations".

#include "render/post/PublicRenderPost.h"
#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

// Upscaler stubs
#include "render/post/upscale/IUpscaler.h"
#include "render/post/upscale/dlss.h"
#include "render/post/upscale/fsr3.h"
#include "render/post/upscale/xess.h"
#include "render/post/upscale/metalfx.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace psynder::render::post {

// ─── Tuning constants ──────────────────────────────────────────────────

// Bloom mip pyramid depth. mip[0] is full-res scene_color, mip[1] is
// half-res after the prefilter, and the chain progressively halves.
// 6 mips for a 4096x4096 source bottoms out at 128x128 -- enough kernel
// width for theatrical bloom without rebuilding the pyramid for
// alternate strengths. Lane 11 keeps this fixed; bloom strength is the
// per-frame tunable via PostDesc::bloom_strength.
static constexpr std::uint32_t kBloomMips = 6u;

// Compute thread-group dims must match the [numthreads(8,8,1)] in
// bloom.slang. Keep these in lockstep when tuning either side.
static constexpr std::uint32_t kBloomTGX = 8u;
static constexpr std::uint32_t kBloomTGY = 8u;

// Push-constant struct shared between PostProcess.cpp (host) and
// bloom.slang. Layout must match BloomPush in the shader exactly.
struct BloomPush
{
    float        src_inv_size[2]; // 1.0 / src_size  (x, y)
    float        dst_inv_size[2]; // 1.0 / dst_size
    std::uint32_t dst_size[2];    // dst width, height
    float        threshold;
    float        strength;
};
static_assert(sizeof(BloomPush) == 32, "BloomPush must be 32 bytes for push_constants");

namespace {

// ─── Internal POD state ───────────────────────────────────────────────

struct TonemapParams
{
    float exposure_ev = 0.0f;
    float _pad0       = 0.0f;
    float _pad1       = 0.0f;
    float _pad2       = 0.0f;
};

struct UICompositeParams
{
    std::uint32_t has_ui_rml = 0u;
    std::uint32_t has_ui_imm = 0u;
    float         _pad0      = 0.0f;
    float         _pad1      = 0.0f;
};

struct State
{
    PostDesc desc{};

    // Per-mip texture handles, indexed [0..kBloomMips-1].
    // mip[0] is full-res; mip[i] is ceil(prev_dim / 2).
    gpu::Handle<gpu::Texture> bloom_mip[kBloomMips];
    std::uint32_t             bloom_mip_w[kBloomMips] = {};
    std::uint32_t             bloom_mip_h[kBloomMips] = {};

    // Bloom compute pipelines.
    shader::PipelineHandle pipe_bloom_prefilter{};
    shader::PipelineHandle pipe_bloom_downsample{};
    shader::PipelineHandle pipe_bloom_upsample{};
    shader::PipelineHandle pipe_bloom_composite{};

    // Tonemap uniform + pipeline.
    gpu::Handle<gpu::Buffer> tonemap_ub;
    TonemapParams            tonemap_params{};
    shader::PipelineHandle   pipe_tonemap{};

    // UI composite uniform + pipeline.
    gpu::Handle<gpu::Buffer> ui_composite_ub;
    UICompositeParams        ui_params{};
    shader::PipelineHandle   pipe_ui_composite{};

    // Intermediate render targets.
    gpu::Handle<gpu::Texture> hdr_bloom_out;   // scene + bloom * strength (HDR)
    gpu::Handle<gpu::Texture> tonemap_out;     // ACES output (sRGB8) pre-UI

    // Externally-bound inputs. Set by the host each frame via
    // set_scene_color() / set_ui_textures() (lane-internal -- not part of
    // the frozen public header).
    gpu::Texture* scene_color = nullptr;
    gpu::Texture* ui_rml      = nullptr;
    gpu::Texture* ui_imm      = nullptr;

    // ── Upscalers ─────────────────────────────────────────────────────
    Upscaler          active_upscaler = Upscaler::Off;
    upscale::Dlss*    dlss            = nullptr;
    upscale::Fsr3*    fsr3            = nullptr;
    upscale::Xess*    xess            = nullptr;
    upscale::MetalFx* metalfx         = nullptr;

    gpu::Device* device = nullptr;
    bool         ready  = false;
};

State g_state;

// ─── Helpers ──────────────────────────────────────────────────────────

template <typename T>
void upload_uniform(gpu::Buffer* buf, const T& data)
{
    if (!buf) return;
    void* ptr = gpu::buffer_map(buf);
    if (ptr) {
        std::memcpy(ptr, &data, sizeof(T));
        gpu::buffer_unmap(buf);
    }
}

upscale::IUpscaler* active_upscaler_ptr()
{
    switch (g_state.active_upscaler) {
        case Upscaler::Dlss:    return g_state.dlss;
        case Upscaler::Fsr3:    return g_state.fsr3;
        case Upscaler::Xess:    return g_state.xess;
        case Upscaler::MetalFx: return g_state.metalfx;
        case Upscaler::Off:     [[fallthrough]];
        default:                return nullptr;
    }
}

std::uint32_t div_up(std::uint32_t x, std::uint32_t d)
{
    return (x + d - 1u) / d;
}

// Allocate the bloom pyramid texture chain. mip[0] is full-res. Each
// subsequent mip is ceil(prev / 2), down to a 1x1 floor.
void create_bloom_pyramid(gpu::Device* device,
                          std::uint32_t base_w, std::uint32_t base_h)
{
    std::uint32_t w = base_w;
    std::uint32_t h = base_h;
    for (std::uint32_t i = 0; i < kBloomMips; ++i) {
        w = std::max<std::uint32_t>(1u, w);
        h = std::max<std::uint32_t>(1u, h);

        gpu::TextureDesc d{};
        d.width      = w;
        d.height     = h;
        d.format     = gpu::Format::Rgba16Float;
        d.usage      = static_cast<std::uint32_t>(gpu::TextureUsage::Storage)
                     | static_cast<std::uint32_t>(gpu::TextureUsage::Sampled);
        const char* names[kBloomMips] = {
            "bloom_mip0", "bloom_mip1", "bloom_mip2",
            "bloom_mip3", "bloom_mip4", "bloom_mip5",
        };
        d.debug_name = names[i];

        g_state.bloom_mip[i]   = gpu::create_texture(device, d);
        g_state.bloom_mip_w[i] = w;
        g_state.bloom_mip_h[i] = h;

        w = (w + 1u) >> 1u;
        h = (h + 1u) >> 1u;
    }
}

// Populate a BloomPush for a src-mip -> dst-mip dispatch.
void make_push(BloomPush& p,
               std::uint32_t src_w, std::uint32_t src_h,
               std::uint32_t dst_w, std::uint32_t dst_h,
               float threshold, float strength)
{
    p.src_inv_size[0] = (src_w > 0u) ? 1.0f / static_cast<float>(src_w) : 0.0f;
    p.src_inv_size[1] = (src_h > 0u) ? 1.0f / static_cast<float>(src_h) : 0.0f;
    p.dst_inv_size[0] = (dst_w > 0u) ? 1.0f / static_cast<float>(dst_w) : 0.0f;
    p.dst_inv_size[1] = (dst_h > 0u) ? 1.0f / static_cast<float>(dst_h) : 0.0f;
    p.dst_size[0]     = dst_w;
    p.dst_size[1]     = dst_h;
    p.threshold       = threshold;
    p.strength        = strength;
}

} // anonymous namespace

// ─── Public API ────────────────────────────────────────────────────────

void init(const PostDesc& desc)
{
    g_state      = {};
    g_state.desc = desc;

    // Lane 07 (gpu) owns the device; we borrow a raw pointer here. For M2
    // the game loop has not been wired, so the device stays nullptr and
    // run_frame() becomes a no-op. Once lane-07 integration lands, the
    // engine passes the device in via a lane-internal setter (TBD with the
    // game loop owner).
    gpu::Device* device = nullptr; // TODO lane-07 integration: pass real device
    g_state.device = device;

    g_state.tonemap_params.exposure_ev = desc.exposure_ev;

    // Always create all 4 upscaler stubs; only the active one is used.
    g_state.dlss    = new upscale::Dlss   (device);
    g_state.fsr3    = new upscale::Fsr3   (device);
    g_state.xess    = new upscale::Xess   (device);
    g_state.metalfx = new upscale::MetalFx(device);
    set_upscaler_runtime(desc.upscaler);

    // Skip GPU resource creation in headless mode. ready stays false so
    // run_frame() short-circuits without dereferencing null pointers.
    if (!device) {
        g_state.ready = false;
        return;
    }

    // ── Bloom pyramid ─────────────────────────────────────────────────
    //
    // mip[0] mirrors the scene resolution (we don't actually write to it;
    // it's a slot in the layout for clarity). The prefilter writes mip[1].
    create_bloom_pyramid(device, desc.output_width, desc.output_height);

    // ── Intermediate render targets ───────────────────────────────────
    gpu::TextureDesc hdr_desc{};
    hdr_desc.width      = desc.output_width;
    hdr_desc.height     = desc.output_height;
    hdr_desc.format     = gpu::Format::Rgba16Float;
    hdr_desc.usage      = static_cast<std::uint32_t>(gpu::TextureUsage::RenderTarget)
                        | static_cast<std::uint32_t>(gpu::TextureUsage::Sampled)
                        | static_cast<std::uint32_t>(gpu::TextureUsage::Storage);
    hdr_desc.debug_name = "hdr_bloom_out";
    g_state.hdr_bloom_out = gpu::create_texture(device, hdr_desc);

    gpu::TextureDesc sdr_desc{};
    sdr_desc.width      = desc.output_width;
    sdr_desc.height     = desc.output_height;
    sdr_desc.format     = gpu::Format::Rgba8Srgb;
    sdr_desc.usage      = static_cast<std::uint32_t>(gpu::TextureUsage::RenderTarget)
                        | static_cast<std::uint32_t>(gpu::TextureUsage::Sampled);
    sdr_desc.debug_name = "tonemap_out";
    g_state.tonemap_out = gpu::create_texture(device, sdr_desc);

    // ── Tonemap / UI composite uniform buffers ────────────────────────
    gpu::BufferDesc ub_desc{};
    ub_desc.size_bytes = 64; // ample headroom for either param struct
    ub_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Uniform);
    ub_desc.heap       = gpu::HeapKind::HostVisible;

    ub_desc.debug_name = "tonemap_ub";
    g_state.tonemap_ub = gpu::create_buffer(device, ub_desc);
    upload_uniform(g_state.tonemap_ub.get(), g_state.tonemap_params);

    ub_desc.debug_name      = "ui_composite_ub";
    g_state.ui_composite_ub = gpu::create_buffer(device, ub_desc);
    upload_uniform(g_state.ui_composite_ub.get(), g_state.ui_params);

    // ── Shader pipelines ──────────────────────────────────────────────
    shader::ComputePipelineDesc bloom_cs{};
    bloom_cs.slang_path = "engine/render/post/shaders/bloom.slang";

    bloom_cs.entry_point_cs       = "cs_bloom_prefilter";
    g_state.pipe_bloom_prefilter  = shader::create_compute(bloom_cs);
    bloom_cs.entry_point_cs       = "cs_bloom_downsample";
    g_state.pipe_bloom_downsample = shader::create_compute(bloom_cs);
    bloom_cs.entry_point_cs       = "cs_bloom_upsample";
    g_state.pipe_bloom_upsample   = shader::create_compute(bloom_cs);
    bloom_cs.entry_point_cs       = "cs_bloom_composite";
    g_state.pipe_bloom_composite  = shader::create_compute(bloom_cs);

    shader::GraphicsPipelineDesc tonemap_gfx{};
    tonemap_gfx.slang_path         = "engine/render/post/shaders/tonemap.slang";
    tonemap_gfx.entry_point_vs     = "vs_tonemap";
    tonemap_gfx.entry_point_fs     = "fs_tonemap";
    tonemap_gfx.enable_depth_write = false;
    tonemap_gfx.color_format_count = 1;
    tonemap_gfx.color_formats[0]   = static_cast<std::uint8_t>(gpu::Format::Rgba8Srgb);
    g_state.pipe_tonemap           = shader::create_graphics(tonemap_gfx);

    shader::GraphicsPipelineDesc ui_gfx{};
    ui_gfx.slang_path         = "engine/render/post/shaders/ui_composite.slang";
    ui_gfx.entry_point_vs     = "vs_ui_composite";
    ui_gfx.entry_point_fs     = "fs_ui_composite";
    ui_gfx.enable_depth_write = false;
    ui_gfx.enable_blend       = false; // alpha blend computed in shader (straight alpha)
    ui_gfx.color_format_count = 1;
    ui_gfx.color_formats[0]   = static_cast<std::uint8_t>(gpu::Format::Bgra8Srgb);
    g_state.pipe_ui_composite = shader::create_graphics(ui_gfx);

    g_state.ready = true;
}

void shutdown()
{
    if (g_state.device) {
        shader::destroy_pipeline(g_state.pipe_bloom_prefilter);
        shader::destroy_pipeline(g_state.pipe_bloom_downsample);
        shader::destroy_pipeline(g_state.pipe_bloom_upsample);
        shader::destroy_pipeline(g_state.pipe_bloom_composite);
        shader::destroy_pipeline(g_state.pipe_tonemap);
        shader::destroy_pipeline(g_state.pipe_ui_composite);
        // gpu::Handle destructors release GPU resources via intrusive refcount.
    }

    delete g_state.dlss;
    delete g_state.fsr3;
    delete g_state.xess;
    delete g_state.metalfx;

    g_state = {}; // resets all Handles, clears externally-bound pointers
}

// Update the runtime tonemap exposure. The frozen header exposes
// PostDesc::exposure_ev at init time; this is the dynamic setter used by
// camera auto-exposure / the r_exposure cvar. Defined here for
// discoverability (not in the frozen public header per AGENTS.md).
void set_exposure_ev(float ev)
{
    g_state.tonemap_params.exposure_ev = ev;
}

// Lane-internal setters: the host (game loop) wires the scene color and
// optional UI overlays each frame. The frozen PublicRenderPost.h doesn't
// expose these; lane 11 owns the cross-lane handshake until the orchestrator
// promotes them into the public header.
void set_scene_color(gpu::Texture* tex)  { g_state.scene_color = tex; }
void set_ui_textures(gpu::Texture* rml, gpu::Texture* imm)
{
    g_state.ui_rml = rml;
    g_state.ui_imm = imm;
    g_state.ui_params.has_ui_rml = (rml != nullptr) ? 1u : 0u;
    g_state.ui_params.has_ui_imm = (imm != nullptr) ? 1u : 0u;
}

void run_frame()
{
    if (!g_state.ready || !g_state.device) {
        // Headless / pre-integration path: no-op so sample_00_clear keeps
        // its M0 behavior when PostDesc was never init()'d.
        return;
    }

    // ── Update per-frame uniforms (small CPU writes; no allocation) ───
    upload_uniform(g_state.tonemap_ub.get(),     g_state.tonemap_params);
    upload_uniform(g_state.ui_composite_ub.get(), g_state.ui_params);

    gpu::CmdBuffer* cmd = gpu::cmd_open(g_state.device);
    if (!cmd) return;

    const std::uint32_t W = g_state.desc.output_width;
    const std::uint32_t H = g_state.desc.output_height;
    const float strength  = g_state.desc.bloom_strength;
    const float threshold = 1.0f; // configurable; default per task spec

    BloomPush push{};

    // ── 1. Bloom prefilter: scene_color (mip[0]) -> mip[1] ────────────
    if (g_state.pipe_bloom_prefilter.valid() && kBloomMips >= 2u) {
        make_push(push,
                  g_state.bloom_mip_w[0], g_state.bloom_mip_h[0],
                  g_state.bloom_mip_w[1], g_state.bloom_mip_h[1],
                  threshold, strength);
        gpu::bind_pipeline(cmd, g_state.pipe_bloom_prefilter);
        gpu::push_constants(cmd, &push, sizeof(push), gpu::ShaderStage::Compute);
        gpu::dispatch(cmd,
            div_up(g_state.bloom_mip_w[1], kBloomTGX),
            div_up(g_state.bloom_mip_h[1], kBloomTGY),
            1u);
    }

    // ── 2. Bloom downsample chain mip[N] -> mip[N+1] for N=1..mips-2 ──
    for (std::uint32_t i = 1u; i + 1u < kBloomMips; ++i) {
        const std::uint32_t dst = i + 1u;
        make_push(push,
                  g_state.bloom_mip_w[i],   g_state.bloom_mip_h[i],
                  g_state.bloom_mip_w[dst], g_state.bloom_mip_h[dst],
                  0.0f, 0.0f);
        gpu::bind_pipeline(cmd, g_state.pipe_bloom_downsample);
        gpu::push_constants(cmd, &push, sizeof(push), gpu::ShaderStage::Compute);
        gpu::dispatch(cmd,
            div_up(g_state.bloom_mip_w[dst], kBloomTGX),
            div_up(g_state.bloom_mip_h[dst], kBloomTGY),
            1u);
    }

    // ── 3. Bloom upsample chain mip[N+1] -> mip[N] for N=mips-2..1 ────
    //
    // Additive accumulation. The host RELIES on cs_bloom_upsample doing
    // read-modify-write -- the destination mip carries the partial bloom
    // from previous (deeper) upsamples; we add this level's tent on top.
    for (std::uint32_t i = kBloomMips - 1u; i >= 2u; --i) {
        const std::uint32_t src = i;
        const std::uint32_t dst = i - 1u;
        make_push(push,
                  g_state.bloom_mip_w[src], g_state.bloom_mip_h[src],
                  g_state.bloom_mip_w[dst], g_state.bloom_mip_h[dst],
                  0.0f, 0.0f);
        gpu::bind_pipeline(cmd, g_state.pipe_bloom_upsample);
        gpu::push_constants(cmd, &push, sizeof(push), gpu::ShaderStage::Compute);
        gpu::dispatch(cmd,
            div_up(g_state.bloom_mip_w[dst], kBloomTGX),
            div_up(g_state.bloom_mip_h[dst], kBloomTGY),
            1u);
    }

    // ── 4. Bloom composite: scene + mip[1] * strength -> hdr_bloom_out ──
    if (g_state.pipe_bloom_composite.valid()) {
        make_push(push,
                  W, H,
                  W, H,
                  0.0f, strength);
        gpu::bind_pipeline(cmd, g_state.pipe_bloom_composite);
        gpu::push_constants(cmd, &push, sizeof(push), gpu::ShaderStage::Compute);
        gpu::dispatch(cmd,
            div_up(W, kBloomTGX),
            div_up(H, kBloomTGY),
            1u);
    }

    // ── 5. Upscale (optional; off by default) ─────────────────────────
    upscale::IUpscaler* up = active_upscaler_ptr();
    if (up && up->is_supported()) {
        upscale::UpscaleInput uin{};
        uin.color  = g_state.hdr_bloom_out.get();
        uin.output = g_state.tonemap_out.get(); // post-upscale handoff
        up->evaluate(uin);
    }

    // ── 6. Tonemap (ACES Narkowicz 2015 + exposure) ───────────────────
    if (g_state.pipe_tonemap.valid()) {
        gpu::RenderPassDesc rp{};
        rp.swapchain     = false;
        rp.color_count   = 1;
        rp.colors[0].target = g_state.tonemap_out.get();
        rp.colors[0].load   = gpu::LoadOp::DontCare;
        rp.colors[0].store  = gpu::StoreOp::Store;

        gpu::begin_render(cmd, rp);
        gpu::Viewport vp{ 0.0f, 0.0f,
                          static_cast<float>(W), static_cast<float>(H),
                          0.0f, 1.0f };
        gpu::set_viewport(cmd, vp);
        gpu::Scissor sc{ 0, 0, W, H };
        gpu::set_scissor(cmd, sc);
        gpu::bind_pipeline(cmd, g_state.pipe_tonemap);
        gpu::draw(cmd, 3, 1, 0, 0); // full-screen triangle
        gpu::end_render(cmd);
    }

    // ── 7. UI composite (skips alpha-blend if no UI textures bound) ───
    if (g_state.pipe_ui_composite.valid()) {
        gpu::RenderPassDesc rp{};
        rp.swapchain     = true;
        rp.color_count   = 1;
        rp.colors[0].target = nullptr; // swapchain image
        rp.colors[0].load   = gpu::LoadOp::DontCare;
        rp.colors[0].store  = gpu::StoreOp::Store;

        gpu::begin_render(cmd, rp);
        gpu::Viewport vp{ 0.0f, 0.0f,
                          static_cast<float>(W), static_cast<float>(H),
                          0.0f, 1.0f };
        gpu::set_viewport(cmd, vp);
        gpu::Scissor sc{ 0, 0, W, H };
        gpu::set_scissor(cmd, sc);
        gpu::bind_pipeline(cmd, g_state.pipe_ui_composite);
        gpu::draw(cmd, 3, 1, 0, 0);
        gpu::end_render(cmd);
    }

    gpu::cmd_submit(g_state.device, cmd);
}

void set_upscaler_runtime(Upscaler selector)
{
    g_state.active_upscaler = selector;

    // Validate: if the requested upscaler is not supported, fall back to Off.
    upscale::IUpscaler* up = active_upscaler_ptr();
    if (up && !up->is_supported()) {
        g_state.active_upscaler = Upscaler::Off;
    }
}

} // namespace psynder::render::post
