// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/asset/GxStubs.cpp
//
// Wave A: trivial no-op implementations of the GX cooker contracts
// declared in TextureCookerGx.h / MeshletCookerGx.h / ShaderCookerGx.h.
// Lane agents in Wave B replace these with real implementations:
//
//   - Lane 05-asset (substantive Wave B work) implements the texture
//     cookers via bc7enc / ispc_texcomp / ASTC encoder, and the meshlet
//     cooker via meshoptimizer's meshopt_buildMeshlets().
//   - Lane 08-shader implements the Slang compiler wrappers.
//
// Keeping the stubs ensures downstream lanes can call the API and the
// build links cleanly; calling these functions in Wave A just returns
// empty results.

#include "asset/TextureCookerGx.h"
#include "asset/MeshletCookerGx.h"
#include "asset/ShaderCookerGx.h"

namespace psynder::asset {

// ─── Texture cooker stubs ───────────────────────────────────────────────
GxCookedTexture cook_bc1(const GxRgba8Image& src, GxCookQuality, std::uint32_t) {
    GxCookedTexture out;
    out.format  = GxBlockFormat::Bc1;
    out.is_srgb = src.is_srgb;
    return out;
}
GxCookedTexture cook_bc3(const GxRgba8Image& src, GxCookQuality, std::uint32_t) {
    GxCookedTexture out;
    out.format  = GxBlockFormat::Bc3;
    out.is_srgb = src.is_srgb;
    return out;
}
GxCookedTexture cook_bc7(const GxRgba8Image& src, GxCookQuality, std::uint32_t) {
    GxCookedTexture out;
    out.format  = GxBlockFormat::Bc7;
    out.is_srgb = src.is_srgb;
    return out;
}
GxCookedTexture cook_astc(const GxRgba8Image& src, GxBlockFormat variant,
                          GxCookQuality, std::uint32_t) {
    GxCookedTexture out;
    out.format  = variant;
    out.is_srgb = src.is_srgb;
    return out;
}

std::size_t gx_cooked_texture_total_bytes(const GxCookedTexture& t) noexcept {
    std::size_t total = 0;
    for (const auto& m : t.mips) total += m.bytes.size();
    return total;
}

// ─── Meshlet cooker stubs ───────────────────────────────────────────────
GxCookedMeshlets cook_meshlets(const GxMeshSrc&, std::uint32_t, std::uint32_t, float) {
    return {};
}
GxCookedMeshlets make_passthrough_meshlet(const GxMeshSrc& src) {
    GxCookedMeshlets out;
    if (src.indices.empty() || src.vertices.empty()) return out;
    // Single meshlet covering the whole mesh; used for fallback rendering
    // when mesh shaders are unavailable. Wave B replaces with the real
    // builder.
    GxMeshlet m{};
    m.vertex_offset    = 0;
    m.vertex_count     = static_cast<std::uint32_t>(src.vertices.size());
    m.triangle_offset  = 0;
    m.triangle_count   = static_cast<std::uint32_t>(src.indices.size() / 3);
    m.bounds.radius    = 0.0f;
    m.bounds.cone_cutoff = 1.0f;
    out.meshlets.push_back(m);
    return out;
}

// ─── Shader compiler stubs ──────────────────────────────────────────────
std::vector<GxCompiledBlob> compile_slang_to_spirv(const GxShaderSource&,
                                                   GxShaderCompileLog* out_log) {
    if (out_log) {
        out_log->ok  = false;
        out_log->log = "stub: lane 08-shader will implement compile_slang_to_spirv()";
    }
    return {};
}
std::vector<GxCompiledBlob> compile_slang_to_metal_ir(const GxShaderSource&,
                                                      GxShaderCompileLog* out_log) {
    if (out_log) {
        out_log->ok  = false;
        out_log->log = "stub: lane 08-shader will implement compile_slang_to_metal_ir()";
    }
    return {};
}
bool gx_shader_can_compile_target(GxShaderTarget) { return false; } // until lane 08 lands

} // namespace psynder::asset
