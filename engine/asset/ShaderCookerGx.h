// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/asset/ShaderCookerGx.h
//
// Wave A FROZEN public-header CONTRACT (lane 05-asset).
//
// Slang → (SPIR-V for Vulkan) and (Metal IR for Apple Silicon) compiler
// wrappers. Implementations are filled in by lane 08-shader (Wave B). The
// asset cooker invokes these at offline cook time; the runtime shader
// hot-reload watcher in lane 08 also invokes them between frames when a
// .slang source changes.
//
// Per DESIGN-PSYNDER-GX.md §2.2 (Slang or fallback glslc+spirv-cross),
// §7.7 (materials are Slang fragment shaders), §10.7 (shader cook step).

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace psynder::asset {

enum class GxShaderStage : std::uint8_t {
    Vertex, Fragment, Compute, Mesh, Task,
    RayGen, RayMiss, RayHitClosest, RayHitAny,
};

struct GxShaderEntry {
    std::string_view name;      // e.g. "vs_main"
    GxShaderStage    stage;
};

// One Slang source file may declare multiple entry points; the cooker
// emits one blob per entry.
struct GxShaderSource {
    std::string_view  slang_path;      // file path relative to VFS root
    std::vector<char> source_text;     // resolved + preprocessed text (caller fills)
    std::vector<GxShaderEntry> entries;
};

enum class GxShaderTarget : std::uint8_t {
    SpirV,      // Vulkan (Win / Linux)
    MetalIr,    // Apple Silicon (macOS)
};

// One compiled blob per entry point per target.
struct GxCompiledBlob {
    std::string_view  entry_name;
    GxShaderStage     stage;
    GxShaderTarget    target;
    std::vector<std::uint8_t> bytes; // SPIR-V words OR Metal IR
};

struct GxShaderCompileLog {
    bool        ok = false;
    std::string log; // human-readable diagnostic output
};

// ─── Compiler entry points (STUBS — lane 08-shader implementation) ───────
//
// compile_slang_to_spirv: invoke the Slang compiler with the SPIR-V back
// end. Emits one blob per declared entry. Stub returns empty blobs + a
// log saying "stub: lane 08-shader will implement".
std::vector<GxCompiledBlob> compile_slang_to_spirv (const GxShaderSource&,
                                                    GxShaderCompileLog* out_log = nullptr);

// compile_slang_to_metal_ir: same, but Metal IR back end. Used on macOS
// Apple Silicon (native Metal path; no MoltenVK).
std::vector<GxCompiledBlob> compile_slang_to_metal_ir(const GxShaderSource&,
                                                      GxShaderCompileLog* out_log = nullptr);

// ─── Helpers ─────────────────────────────────────────────────────────────
// True if the current host can compile to this target. Always true for
// SPIR-V on any host (Slang runs cross-platform). For Metal IR, requires
// macOS + Xcode toolchain (Slang uses Apple's Metal compiler on the back end).
bool gx_shader_can_compile_target(GxShaderTarget);

} // namespace psynder::asset
