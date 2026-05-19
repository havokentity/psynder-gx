// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/post/PostProcess.cpp
//
// Lane 11 — post-process stack: bloom, ACES tonemap, upscaler dispatch,
// and final UI compositing pass.
//
// Public API: psynder::render::post::{init, shutdown, run_frame,
//             set_upscaler_runtime, set_exposure_ev}
// See PublicRenderPost.h for the frozen contract.
//
// Pipeline executed each frame (in order):
//
//   1. Bloom  — 4 passes:
//        a. cs_threshold  (downsample + bright-pass, half-res)
//        b. cs_blur_h     (horizontal Gaussian, half-res)
//        c. cs_blur_v     (vertical Gaussian, half-res)
//        d. fs_composite  (additive blend onto scene)
//
//   2. Upscale — delegates to active IUpscaler (always Off at M2).
//
//   3. Tonemap — ACES filmic + exposure EV, fullscreen pass.
//
//   4. UI composite — RmlUi atlas + immediate overlay onto tonemapped image.
//
// All GPU resources are allocated at init() and freed at shutdown().
// No heap allocations in run_frame() (per DESIGN §14 no-alloc in frame loop).

#include "render/post/PublicRenderPost.h"
#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

// Upscaler stubs
#include "render/post/upscale/IUpscaler.h"
#include "render/post/upscale/dlss.h"
#include "render/post/upscale/fsr3.h"
#include "render/post/upscale/xess.h"
#include "render/post/upscale/metalfx.h"

#include <cstdint>
#include <cstring>

namespace psynder::render::post {

// ── State (module-level, no dynamic allocation after init) ────────────────

namespace {

struct BloomParams
{
    float    threshold  = 1.0f;
    float    strength   = 0.04f;
    uint32_t src_width  = 0;
    uint32_t src_height = 0;
};

struct TonemapParams
{
    float exposure_ev = 0.0f;
    float _pad0       = 0.0f;
    float _pad1       = 0.0f;
    float _pad2       = 0.0f;
};

struct UICompositeParams
{
    uint32_t has_ui_rml = 0;
    uint32_t has_ui_imm = 0;
    float    _pad0      = 0.0f;
    float    _pad1      = 0.0f;
};

struct State
{
    PostDesc desc{};

    // ── Bloom ─────────────────────────────────────────────────────────
    // Two scratch buffers at half-res for ping-pong blur.
    gpu::Handle<gpu::Texture> bloom_scratch_a; // threshold write / blur-h read
    gpu::Handle<gpu::Texture> bloom_scratch_b; // blur-h write  / blur-v write

    gpu::Handle<gpu::Buffer>  bloom_ub;        // uniform buffer
    BloomParams               bloom_params{};

    shader::PipelineHandle    pipe_bloom_threshold{};
    shader::PipelineHandle    pipe_bloom_blur_h{};
    shader::PipelineHandle    pipe_bloom_blur_v{};
    shader::PipelineHandle    pipe_bloom_composite{};

    // ── Tonemap ───────────────────────────────────────────────────────
    gpu::Handle<gpu::Buffer>  tonemap_ub;
    TonemapParams             tonemap_params{};
    shader::PipelineHandle    pipe_tonemap{};

    // Intermediate target: bloom-composited but not yet tonemapped.
    gpu::Handle<gpu::Texture> hdr_bloom_out;

    // ── Upscalers ─────────────────────────────────────────────────────
    Upscaler active_upscaler = Upscaler::Off;

    upscale::Dlss*    dlss    = nullptr;
    upscale::Fsr3*    fsr3    = nullptr;
    upscale::Xess*    xess    = nullptr;
    upscale::MetalFx* metalfx = nullptr;

    // ── UI composite ──────────────────────────────────────────────────
    gpu::Handle<gpu::Buffer>  ui_composite_ub;
    UICompositeParams         ui_params{};
    shader::PipelineHandle    pipe_ui_composite{};

    // Tonemapped output texture (pre-UI-composite).
    gpu::Handle<gpu::Texture> tonemap_out;

    gpu::Device* device = nullptr;
    bool         ready  = false;
};

State g_state;

// ── Helpers ───────────────────────────────────────────────────────────────

// Write a plain struct into a HostVisible buffer (replaces entire contents).
template <typename T>
static void upload_uniform(gpu::Buffer* buf, const T& data)
{
    void* ptr = gpu::buffer_map(buf);
    if (ptr) {
        std::memcpy(ptr, &data, sizeof(T));
        gpu::buffer_unmap(buf);
    }
}

// Return the IUpscaler matching the selector (may be null if selector is Off).
static upscale::IUpscaler* active_upscaler_ptr()
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

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────

void init(const PostDesc& desc)
{
    g_state      = {};
    g_state.desc = desc;

    // Lane 07 (gpu) owns the device; we borrow a raw pointer here.
    // The real host (samples / game loop) passes it via a lane-07 device ref.
    // For M2 the game loop has not been wired, so device_ stays nullptr and
    // all GPU resource creation is skipped — run_frame() becomes a no-op.
    //
    // Integration point: replace the nullptr below with the device handle
    // retrieved from the lane-07 initialization call.
    gpu::Device* device = nullptr; // TODO lane-07 integration: pass real device
    g_state.device = device;

    // ── Bloom uniform buffer ──────────────────────────────────────────────
    g_state.bloom_params.threshold  = 1.0f;
    g_state.bloom_params.strength   = desc.bloom_strength;
    g_state.bloom_params.src_width  = desc.output_width;
    g_state.bloom_params.src_height = desc.output_height;

    // ── Tonemap uniform buffer ────────────────────────────────────────────
    g_state.tonemap_params.exposure_ev = desc.exposure_ev;

    // ── Upscalers — always create all 4 stubs; only one will be active ───
    // Stubs are cheap (no SDK init until M9 real integration).
    g_state.dlss    = new upscale::Dlss   (device);
    g_state.fsr3    = new upscale::Fsr3   (device);
    g_state.xess    = new upscale::Xess   (device);
    g_state.metalfx = new upscale::MetalFx(device);

    // Apply the requested upscaler (will fall back to Off if unsupported).
    set_upscaler_runtime(desc.upscaler);

    // ── GPU resource + pipeline creation (skipped when device is null) ────
    if (!device) {
        g_state.ready = false;
        return;
    }

    // Half-resolution bloom scratch textures (Rgba16Float, Storage+RenderTarget).
    const uint32_t hw = (desc.output_width  + 1u) >> 1u;
    const uint32_t hh = (desc.output_height + 1u) >> 1u;

    gpu::TextureDesc bloom_scratch_desc{};
    bloom_scratch_desc.width      = hw;
    bloom_scratch_desc.height     = hh;
    bloom_scratch_desc.format     = gpu::Format::Rgba16Float;
    bloom_scratch_desc.usage      = static_cast<uint32_t>(gpu::TextureUsage::Storage)
                                  | static_cast<uint32_t>(gpu::TextureUsage::Sampled);
    bloom_scratch_desc.debug_name = "bloom_scratch_a";
    g_state.bloom_scratch_a = gpu::create_texture(device, bloom_scratch_desc);

    bloom_scratch_desc.debug_name = "bloom_scratch_b";
    g_state.bloom_scratch_b = gpu::create_texture(device, bloom_scratch_desc);

    // HDR bloom-out (full resolution, scene + bloom before tonemap).
    gpu::TextureDesc hdr_desc{};
    hdr_desc.width      = desc.output_width;
    hdr_desc.height     = desc.output_height;
    hdr_desc.format     = gpu::Format::Rgba16Float;
    hdr_desc.usage      = static_cast<uint32_t>(gpu::TextureUsage::RenderTarget)
                        | static_cast<uint32_t>(gpu::TextureUsage::Sampled);
    hdr_desc.debug_name = "hdr_bloom_out";
    g_state.hdr_bloom_out = gpu::create_texture(device, hdr_desc);

    // Tonemap output (full res, sRGB8, feeds UI composite).
    gpu::TextureDesc sdr_desc{};
    sdr_desc.width      = desc.output_width;
    sdr_desc.height     = desc.output_height;
    sdr_desc.format     = gpu::Format::Rgba8Srgb;
    sdr_desc.usage      = static_cast<uint32_t>(gpu::TextureUsage::RenderTarget)
                        | static_cast<uint32_t>(gpu::TextureUsage::Sampled);
    sdr_desc.debug_name = "tonemap_out";
    g_state.tonemap_out = gpu::create_texture(device, sdr_desc);

    // Uniform buffers (HostVisible so the CPU updates them each frame).
    gpu::BufferDesc ub_desc{};
    ub_desc.size_bytes = 64; // enough for any of our param structs
    ub_desc.usage      = static_cast<uint32_t>(gpu::BufferUsage::Uniform);
    ub_desc.heap       = gpu::HeapKind::HostVisible;

    ub_desc.debug_name  = "bloom_ub";
    g_state.bloom_ub    = gpu::create_buffer(device, ub_desc);
    upload_uniform(g_state.bloom_ub.get(), g_state.bloom_params);

    ub_desc.debug_name  = "tonemap_ub";
    g_state.tonemap_ub  = gpu::create_buffer(device, ub_desc);
    upload_uniform(g_state.tonemap_ub.get(), g_state.tonemap_params);

    ub_desc.debug_name       = "ui_composite_ub";
    g_state.ui_composite_ub  = gpu::create_buffer(device, ub_desc);
    upload_uniform(g_state.ui_composite_ub.get(), g_state.ui_params);

    // ── Shader pipelines ─────────────────────────────────────────────────
    // Bloom compute passes.
    shader::ComputePipelineDesc bloom_cs{};
    bloom_cs.slang_path = "engine/render/post/shaders/bloom.slang";

    bloom_cs.entry_point_cs     = "cs_threshold";
    g_state.pipe_bloom_threshold = shader::create_compute(bloom_cs);

    bloom_cs.entry_point_cs     = "cs_blur_h";
    g_state.pipe_bloom_blur_h   = shader::create_compute(bloom_cs);

    bloom_cs.entry_point_cs     = "cs_blur_v";
    g_state.pipe_bloom_blur_v   = shader::create_compute(bloom_cs);

    // Bloom composite graphics pass (adds bloom onto scene color).
    shader::GraphicsPipelineDesc bloom_gfx{};
    bloom_gfx.slang_path          = "engine/render/post/shaders/bloom.slang";
    bloom_gfx.entry_point_vs      = "vs_bloom_composite";
    bloom_gfx.entry_point_fs      = "fs_bloom_composite";
    bloom_gfx.enable_depth_write  = false;
    bloom_gfx.enable_blend        = true;
    bloom_gfx.color_format_count  = 1;
    bloom_gfx.color_formats[0]    = static_cast<uint8_t>(gpu::Format::Rgba16Float);
    g_state.pipe_bloom_composite  = shader::create_graphics(bloom_gfx);

    // Tonemap pass.
    shader::GraphicsPipelineDesc tonemap_gfx{};
    tonemap_gfx.slang_path         = "engine/render/post/shaders/tonemap.slang";
    tonemap_gfx.entry_point_vs     = "vs_tonemap";
    tonemap_gfx.entry_point_fs     = "fs_tonemap";
    tonemap_gfx.enable_depth_write = false;
    tonemap_gfx.color_format_count = 1;
    tonemap_gfx.color_formats[0]   = static_cast<uint8_t>(gpu::Format::Rgba8Srgb);
    g_state.pipe_tonemap           = shader::create_graphics(tonemap_gfx);

    // UI composite pass.
    shader::GraphicsPipelineDesc ui_gfx{};
    ui_gfx.slang_path         = "engine/render/post/shaders/ui_composite.slang";
    ui_gfx.entry_point_vs     = "vs_ui_composite";
    ui_gfx.entry_point_fs     = "fs_ui_composite";
    ui_gfx.enable_depth_write = false;
    ui_gfx.enable_blend       = true;  // pre-mult alpha-over
    ui_gfx.color_format_count = 1;
    ui_gfx.color_formats[0]   = static_cast<uint8_t>(gpu::Format::Bgra8Srgb);
    g_state.pipe_ui_composite = shader::create_graphics(ui_gfx);

    g_state.ready = true;
}

void shutdown()
{
    if (!g_state.device) {
        // Headless or pre-device path: free upscaler stubs, reset state.
    } else {
        shader::destroy_pipeline(g_state.pipe_bloom_threshold);
        shader::destroy_pipeline(g_state.pipe_bloom_blur_h);
        shader::destroy_pipeline(g_state.pipe_bloom_blur_v);
        shader::destroy_pipeline(g_state.pipe_bloom_composite);
        shader::destroy_pipeline(g_state.pipe_tonemap);
        shader::destroy_pipeline(g_state.pipe_ui_composite);
        // gpu::Handle destructors release GPU resources via intrusive refcount.
    }

    delete g_state.dlss;
    delete g_state.fsr3;
    delete g_state.xess;
    delete g_state.metalfx;

    g_state = {};
}

void run_frame()
{
    if (!g_state.ready || !g_state.device) {
        // M2: no-op until lane 07 integration is complete.
        return;
    }

    // ── Update uniforms ───────────────────────────────────────────────────
    upload_uniform(g_state.bloom_ub.get(), g_state.bloom_params);
    upload_uniform(g_state.tonemap_ub.get(), g_state.tonemap_params);
    upload_uniform(g_state.ui_composite_ub.get(), g_state.ui_params);

    gpu::CmdBuffer* cmd = gpu::cmd_open(g_state.device);

    // ── 1. Bloom ──────────────────────────────────────────────────────────
    // Pass 1a: threshold + downsample → scratch_a.
    // (Bind: t_scene_color = scene HDR input, t_bloom_write = scratch_a)
    // Dispatch half-res.
    const uint32_t hw = (g_state.desc.output_width  + 1u) >> 1u;
    const uint32_t hh = (g_state.desc.output_height + 1u) >> 1u;
    const uint32_t tx = (hw + 7u) / 8u;
    const uint32_t ty = (hh + 7u) / 8u;
    // TODO lane-07 integration: cmd->bind_compute_pipeline + dispatch(tx, ty, 1)
    (void)tx; (void)ty;

    // Pass 1b: horizontal blur scratch_a → scratch_b.
    // Pass 1c: vertical blur scratch_b → scratch_a (final bloom mask).
    // Pass 1d: additive composite onto hdr_bloom_out.

    // ── 2. Upscale ────────────────────────────────────────────────────────
    upscale::IUpscaler* up = active_upscaler_ptr();
    if (up && up->is_supported()) {
        upscale::UpscaleInput ui{};
        // TODO lane-07 integration: set ui.color / depth / motion / output.
        up->evaluate(ui);
    }
    // If upscaler is Off or unsupported, the hdr_bloom_out is passed
    // directly to the tonemap pass (no scaling step).

    // ── 3. Tonemap ────────────────────────────────────────────────────────
    // Fullscreen pass: hdr_bloom_out → tonemap_out (sRGB8).
    // TODO lane-07 integration: cmd->begin_render, bind_graphics_pipeline,
    //      draw(3), end_render.

    // ── 4. UI composite ───────────────────────────────────────────────────
    // Fullscreen pass: tonemap_out + [ui_rml] + [ui_imm] → backbuffer.
    // Lane 21 must call set_ui_atlas_rml() / set_ui_atlas_imm() each frame
    // before run_frame() (cross-lane integration TODO — see GitHub Issue).
    // TODO lane-07 integration: cmd->begin_render (backbuffer target),
    //      bind_graphics_pipeline, draw(3), end_render.

    gpu::cmd_submit(g_state.device, cmd);
}

void set_upscaler_runtime(Upscaler selector)
{
    g_state.active_upscaler = selector;

    // Validate: if the requested upscaler is not supported, fall back to Off.
    upscale::IUpscaler* up = active_upscaler_ptr();
    if (up && !up->is_supported()) {
        // Log a warning: requested upscaler unavailable on this GPU.
        // psy::log("render-post: %s not supported on this GPU; falling back to Off",
        //          up->name());
        g_state.active_upscaler = Upscaler::Off;
    }
}

// ── Non-public but cross-lane exposed helper (not in frozen header) ────────
//
// Called by the game loop / engine frame orchestrator to feed per-frame
// exposure (e.g. from camera auto-exposure system in lane 09).
//
// The frozen PublicRenderPost.h exposes PostDesc::exposure_ev at init time.
// A runtime knob is needed for dynamic auto-exposure or the console var
// r_exposure.  This function updates the uniform that the tonemap shader reads.
//
// Integration note: expose via a thin internal header or a lane-internal
// cvar callback.  Defined here for discoverability; not declared in the
// frozen public header per AGENTS.md contract rules.

void set_exposure_ev(float ev)
{
    g_state.tonemap_params.exposure_ev = ev;
}

} // namespace psynder::render::post
