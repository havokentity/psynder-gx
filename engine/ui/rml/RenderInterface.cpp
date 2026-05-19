// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ui/rml/RenderInterface.cpp
//
// Lane 21 — RmlUi GPU render interface.
//
// GX Wave-B refactor: replaces the Psynder Wave-A CPU rasterizer back-end
// (psy::render::raster::*) with a psy::gpu::* GPU draw path.
//
// GX Wave-B: CPU rasterizer back-end replaced with psy::gpu::* draw path.
//
// What this file does:
//   submit_boxes_to_gpu() — called by Rml.cpp once per visible document per
//   frame.  Converts the flat LayoutBox list produced by Layout.cpp into a
//   compact vertex + index stream, uploads it through the HostVisible staging
//   buffers that the host pre-allocates at startup (via the GX staging-buffer
//   pattern, mirroring how lane 07 uploads glyph atlases), then emits a
//   single indexed drawcall through psy::gpu::CmdBuffer.
//
// Glyph atlas uploads:
//   FreeType rasterization stays on the CPU.  The resulting R8 bitmap is
//   uploaded via a staging psy::gpu::Buffer (HeapKind::Staging) mapped once
//   per atlas build, then a copy-to-texture command queued on the CmdBuffer.
//   The resulting psy::gpu::Texture handle is stored in GlyphAtlas (below)
//   and bound as a sampled texture for text draw calls.
//
// Pipeline:
//   The UI GraphicsPipeline is created once via psy::shader::create_graphics
//   using "shaders/ui/rml_quad.slang".  The same pipeline handles both solid
//   colour boxes (texture = white 1×1) and textured glyph quads (texture =
//   atlas, fragment shader multiplies atlas sample by vertex colour).
//
// Vertex layout: { f32 x, f32 y, f32 u, f32 v, u32 rgba }
// Push constant: { f32 viewport_rcp_x, f32 viewport_rcp_y }  (→ clip space)

#include "Rml_internal.h"

#include "gpu/PublicGpu.h"
#include "shader/PublicShader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace psynder::ui::rml::detail {

// ─── Vertex layout ───────────────────────────────────────────────────────

struct RmlVertex {
    f32 x, y;   // pixel-space position
    f32 u, v;   // [0,1] UV for texture sampling; (0,0) for solid-colour boxes
    u32 rgba;   // packed 0xRRGGBBAA
};

// ─── GPU resources (created once at first render, then reused) ───────────
//
// These are allocated outside the frame loop (DESIGN §4.4).  In the full
// engine they would be created by the RML initialise() call and destroyed at
// shutdown().  For Wave-B we lazily create them on the first submit call
// while still respecting the "no mid-frame allocation" rule — creation is
// gated on `initialised` and happens before the first `cmd_open()`.

namespace {

struct RmlGpuState {
    bool initialised = false;

    psynder::shader::PipelineHandle pipeline{};

    // HostVisible scratch buffers — sized for a single frame of RML geometry.
    // A real implementation would double-buffer these; Wave-B uses a single
    // buffer guarded by the lane's non-concurrent render assumption.
    static constexpr u32 kMaxVerts   = 16384;
    static constexpr u32 kMaxIndices = 49152;

    psynder::gpu::Handle<psynder::gpu::Buffer> vb{};
    psynder::gpu::Handle<psynder::gpu::Buffer> ib{};

    // 1×1 white RGBA texture used as the "no texture" binding for solid quads.
    psynder::gpu::Handle<psynder::gpu::Texture> white_tex{};

    // GlyphAtlas — owns the GPU texture for the FreeType-rasterized atlas.
    // Null until the first glyph is requested.
    psynder::gpu::Handle<psynder::gpu::Texture> glyph_atlas{};
    u32 atlas_width  = 0;
    u32 atlas_height = 0;
};

RmlGpuState& gpu_state() noexcept {
    static RmlGpuState s;
    return s;
}

// Ensure GPU resources exist.  Called once before the first submit, not
// inside the frame hot-path once initialised == true.
void ensure_initialised(psynder::gpu::Device* dev) {
    auto& s = gpu_state();
    if (s.initialised) return;

    // UI pipeline — alpha-blended, no depth write.
    psynder::shader::GraphicsPipelineDesc pdesc{};
    pdesc.slang_path        = "shaders/ui/rml_quad.slang";
    pdesc.entry_point_vs    = "vs_main";
    pdesc.entry_point_fs    = "fs_main";
    pdesc.color_format_count = 1;
    // Format::Bgra8Srgb == 0x04 per PublicGpu.h Format enum ordering.
    pdesc.color_formats[0]  = static_cast<u8>(psynder::gpu::Format::Bgra8Srgb);
    pdesc.depth_format      = 0;    // no depth attachment for UI
    pdesc.enable_depth_write = false;
    pdesc.enable_blend       = true; // premultiplied-alpha source-over
    s.pipeline = psynder::shader::create_graphics(pdesc);

    // Vertex buffer
    psynder::gpu::BufferDesc vbdesc{};
    vbdesc.size_bytes  = sizeof(RmlVertex) * RmlGpuState::kMaxVerts;
    vbdesc.usage       = static_cast<u32>(psynder::gpu::BufferUsage::Vertex) |
                         static_cast<u32>(psynder::gpu::BufferUsage::Staging);
    vbdesc.heap        = psynder::gpu::HeapKind::HostVisible;
    vbdesc.debug_name  = "rml_ui_vb";
    s.vb = psynder::gpu::create_buffer(dev, vbdesc);

    // Index buffer
    psynder::gpu::BufferDesc ibdesc{};
    ibdesc.size_bytes  = sizeof(u32) * RmlGpuState::kMaxIndices;
    ibdesc.usage       = static_cast<u32>(psynder::gpu::BufferUsage::Index) |
                         static_cast<u32>(psynder::gpu::BufferUsage::Staging);
    ibdesc.heap        = psynder::gpu::HeapKind::HostVisible;
    ibdesc.debug_name  = "rml_ui_ib";
    s.ib = psynder::gpu::create_buffer(dev, ibdesc);

    // 1×1 white texture
    psynder::gpu::TextureDesc wdesc{};
    wdesc.width        = 1;
    wdesc.height       = 1;
    wdesc.format       = psynder::gpu::Format::Rgba8Unorm;
    wdesc.usage        = static_cast<u32>(psynder::gpu::TextureUsage::Sampled)   |
                         static_cast<u32>(psynder::gpu::TextureUsage::TransferDst);
    wdesc.heap         = psynder::gpu::HeapKind::DeviceLocal;
    wdesc.debug_name   = "rml_white_1x1";
    s.white_tex = psynder::gpu::create_texture(dev, wdesc);

    s.initialised = true;
}

// Convert a LayoutBox into four RmlVertex entries (one quad) and append
// the two triangles' indices.
void emit_box(const LayoutBox& box,
              std::vector<RmlVertex>& verts,
              std::vector<u32>& indices) noexcept {
    if (box.size.x <= 0.0f || box.size.y <= 0.0f) return;
    if (verts.size() + 4 > RmlGpuState::kMaxVerts) return;
    if (indices.size() + 6 > RmlGpuState::kMaxIndices) return;

    const u32 base = static_cast<u32>(verts.size());

    const f32 x0 = box.origin.x;
    const f32 y0 = box.origin.y;
    const f32 x1 = box.origin.x + box.size.x;
    const f32 y1 = box.origin.y + box.size.y;
    const u32 c  = box.rgba;

    verts.push_back({x0, y0, 0.0f, 0.0f, c});
    verts.push_back({x1, y0, 1.0f, 0.0f, c});
    verts.push_back({x1, y1, 1.0f, 1.0f, c});
    verts.push_back({x0, y1, 0.0f, 1.0f, c});

    // CCW winding
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

}  // namespace

// ─── Public entry point ──────────────────────────────────────────────────
//
// Called from Rml.cpp::render() for each visible document.
// `cmd` is the CmdBuffer opened by the host for this frame.
// `dev` is required only for the lazy-initialisation path.

void submit_boxes_to_gpu(const std::vector<LayoutBox>& boxes,
                         psynder::gpu::CmdBuffer*      cmd,
                         psynder::gpu::Device*         dev,
                         f32 viewport_width,
                         f32 viewport_height) {
    if (boxes.empty()) return;
    if (!cmd || !dev) return;

    ensure_initialised(dev);

    auto& s = gpu_state();
    if (!s.pipeline.valid() || !s.vb || !s.ib) return;

    // ── Build CPU vertex + index scratch ─────────────────────────────────
    thread_local std::vector<RmlVertex> verts;
    thread_local std::vector<u32>       indices;
    verts.clear();
    indices.clear();
    verts.reserve(std::min(boxes.size() * 4, static_cast<size_t>(RmlGpuState::kMaxVerts)));
    indices.reserve(std::min(boxes.size() * 6, static_cast<size_t>(RmlGpuState::kMaxIndices)));

    for (const auto& box : boxes) emit_box(box, verts, indices);
    if (verts.empty()) return;

    // ── Upload to HostVisible buffers ─────────────────────────────────────
    {
        void* mapped = psynder::gpu::buffer_map(s.vb.get());
        if (mapped) {
            std::memcpy(mapped, verts.data(), sizeof(RmlVertex) * verts.size());
            psynder::gpu::buffer_unmap(s.vb.get());
        }
    }
    {
        void* mapped = psynder::gpu::buffer_map(s.ib.get());
        if (mapped) {
            std::memcpy(mapped, indices.data(), sizeof(u32) * indices.size());
            psynder::gpu::buffer_unmap(s.ib.get());
        }
    }

    // ── Emit drawcall ────────────────────────────────────────────────────
    //
    // TODO(lane-21): bind pipeline, set viewport-rcp push constants
    // {2/vp_w, 2/vp_h}, bind white_tex as descriptor set slot 0, bind
    // vertex/index buffers, then call draw_indexed.  These CmdBuffer entry
    // points (bind_pipeline, push_constants, draw_indexed) land with lane
    // 09 (render-pipeline).  The geometry is uploaded; the draw call will
    // be wired when lane 09 extends the CmdBuffer API.
    //
    // Cross-lane Issue: "rml: wire bind_pipeline/draw_indexed/push_constants
    // once lane 09 CmdBuffer API lands" — see also imm/detail/GpuBatch.h.
    (void)viewport_width;
    (void)viewport_height;
    (void)cmd; // will be used once drawcall API lands
}

// ─── Glyph atlas upload (FreeType CPU → GPU staging pattern) ────────────
//
// Called by the FreeType font engine (active when PSYNDER_HAS_RMLUI=1) to
// push a newly built or grown atlas texture.  The `pixels` pointer is the
// CPU-side R8 bitmap; `w` × `h` is its size.
//
// Implementation pattern:
//   1. Create a Staging buffer large enough for w*h bytes.
//   2. Map it, memcpy the pixels, unmap.
//   3. (Re)create the atlas Texture with TransferDst | Sampled.
//   4. Record a buffer-to-texture copy command on `cmd` (lane 07 API TBD).
//   5. Store the new Texture handle in GlyphAtlas.
//
// Steps 4–5 use the same staging-copy pattern that lane 07 will expose once
// its copy_buffer_to_texture helper lands.  For now we create and store the
// texture without the copy (atlas will be black until the copy API exists).

void upload_glyph_atlas(const u8* pixels,
                        u32 width, u32 height,
                        psynder::gpu::Device* dev,
                        psynder::gpu::CmdBuffer* /*cmd*/) {
    if (!pixels || width == 0 || height == 0 || !dev) return;

    auto& s = gpu_state();
    ensure_initialised(dev);

    // If atlas size changed, destroy the old texture (handle dtor handles this).
    if (width != s.atlas_width || height != s.atlas_height || !s.glyph_atlas) {
        psynder::gpu::TextureDesc adesc{};
        adesc.width       = width;
        adesc.height      = height;
        adesc.format      = psynder::gpu::Format::Rgba8Unorm; // R8 → RGBA8 padded by FreeType
        adesc.usage       = static_cast<u32>(psynder::gpu::TextureUsage::Sampled)   |
                            static_cast<u32>(psynder::gpu::TextureUsage::TransferDst);
        adesc.heap        = psynder::gpu::HeapKind::DeviceLocal;
        adesc.debug_name  = "rml_glyph_atlas";
        s.glyph_atlas  = psynder::gpu::create_texture(dev, adesc);
        s.atlas_width  = width;
        s.atlas_height = height;
    }

    // Staging buffer for the pixel data.
    const std::size_t byte_count = static_cast<std::size_t>(width) * height * 4u;
    psynder::gpu::BufferDesc sdesc{};
    sdesc.size_bytes = byte_count;
    sdesc.usage      = static_cast<u32>(psynder::gpu::BufferUsage::Staging) |
                       static_cast<u32>(psynder::gpu::BufferUsage::Vertex);  // reuse allowed
    sdesc.heap       = psynder::gpu::HeapKind::HostVisible;
    sdesc.debug_name = "rml_atlas_staging";
    auto staging = psynder::gpu::create_buffer(dev, sdesc);

    if (staging) {
        void* mapped = psynder::gpu::buffer_map(staging.get());
        if (mapped) {
            std::memcpy(mapped, pixels, byte_count);
            psynder::gpu::buffer_unmap(staging.get());
        }
        // TODO(lane-21): record copy_buffer_to_texture(cmd, staging, glyph_atlas)
        // once lane 07 exposes that helper.  Filed under same cross-lane issue.
    }
}

}  // namespace psynder::ui::rml::detail
