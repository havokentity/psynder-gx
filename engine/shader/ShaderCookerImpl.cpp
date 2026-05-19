// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/ShaderCookerImpl.cpp
//
// Lane 08 — Implementation of the compiler entry points declared in
// engine/asset/ShaderCookerGx.h (the frozen public contract from lane 05).
//
// The asset lane's ShaderCookerGx.h declares:
//   compile_slang_to_spirv()
//   compile_slang_to_metal_ir()
//   gx_shader_can_compile_target()
//
// This TU lives in engine/shader/ (lane 08 territory), NOT in engine/asset/.
// The asset lane links psynder_shader to resolve these symbols.
//
// NOTE: DO NOT MODIFY engine/asset/ShaderCookerGx.h — it is lane 05 territory.

#include "asset/ShaderCookerGx.h"
#include "shader/impl/ShaderImpl.h"

#include <cstdio>
#include <string>
#include <vector>

namespace psynder::asset {

// Helper: map GxShaderStage → psynder::shader::Stage
static psynder::shader::Stage to_shader_stage(GxShaderStage s)
{
    using S = psynder::shader::Stage;
    switch (s) {
        case GxShaderStage::Vertex:        return S::Vertex;
        case GxShaderStage::Fragment:      return S::Fragment;
        case GxShaderStage::Compute:       return S::Compute;
        case GxShaderStage::Mesh:          return S::Mesh;
        case GxShaderStage::Task:          return S::Task;
        case GxShaderStage::RayGen:        return S::RayGen;
        case GxShaderStage::RayMiss:       return S::RayMiss;
        case GxShaderStage::RayHitClosest: return S::RayHitClosest;
        case GxShaderStage::RayHitAny:     return S::RayHitAny;
    }
    return S::Vertex;
}

std::vector<GxCompiledBlob> compile_slang_to_spirv(
    const GxShaderSource&   src,
    GxShaderCompileLog*     out_log)
{
    std::vector<GxCompiledBlob> result;

    if (src.slang_path.empty()) {
        if (out_log) { out_log->ok = false; out_log->log = "slang_path is empty"; }
        return result;
    }

    // We need the source on disk (the caller pre-resolves the VFS path).
    const std::string path_str(src.slang_path);

    bool all_ok = true;
    std::string combined_log;

    for (const auto& entry : src.entries) {
        GxCompiledBlob blob;
        blob.entry_name = entry.name;
        blob.stage      = entry.stage;
        blob.target     = GxShaderTarget::SpirV;

        std::string log;
        const bool ok = psynder::shader::impl::compile_to_spirv(
            path_str.c_str(),
            std::string(entry.name).c_str(),
            to_shader_stage(entry.stage),
            blob.bytes,
            &log);

        if (!ok) {
            all_ok = false;
            combined_log += "[spirv] entry ";
            combined_log += entry.name;
            combined_log += ": ";
            combined_log += log;
            combined_log += "\n";
        } else {
            combined_log += "[spirv] entry ";
            combined_log += entry.name;
            combined_log += ": ok (";
            combined_log += std::to_string(blob.bytes.size());
            combined_log += " bytes)\n";
            result.push_back(std::move(blob));
        }
    }

    if (out_log) {
        out_log->ok  = all_ok;
        out_log->log = combined_log;
    }
    return result;
}

std::vector<GxCompiledBlob> compile_slang_to_metal_ir(
    const GxShaderSource&   src,
    GxShaderCompileLog*     out_log)
{
    std::vector<GxCompiledBlob> result;

#if !defined(__APPLE__)
    if (out_log) {
        out_log->ok  = false;
        out_log->log = "Metal IR compilation only supported on macOS";
    }
    return result;
#else
    if (src.slang_path.empty()) {
        if (out_log) { out_log->ok = false; out_log->log = "slang_path is empty"; }
        return result;
    }

    const std::string path_str(src.slang_path);
    bool all_ok = true;
    std::string combined_log;

    for (const auto& entry : src.entries) {
        GxCompiledBlob blob;
        blob.entry_name = entry.name;
        blob.stage      = entry.stage;
        blob.target     = GxShaderTarget::MetalIr;

        std::string log;
        const bool ok = psynder::shader::impl::compile_to_metal_ir(
            path_str.c_str(),
            std::string(entry.name).c_str(),
            to_shader_stage(entry.stage),
            blob.bytes,
            &log);

        if (!ok) {
            all_ok = false;
            combined_log += "[metal] entry ";
            combined_log += entry.name;
            combined_log += ": ";
            combined_log += log;
            combined_log += "\n";
        } else {
            combined_log += "[metal] entry ";
            combined_log += entry.name;
            combined_log += ": ok (";
            combined_log += std::to_string(blob.bytes.size());
            combined_log += " bytes)\n";
            result.push_back(std::move(blob));
        }
    }

    if (out_log) {
        out_log->ok  = all_ok;
        out_log->log = combined_log;
    }
    return result;
#endif
}

bool gx_shader_can_compile_target(GxShaderTarget target)
{
    switch (target) {
        case GxShaderTarget::SpirV:
            // slangc can cross-compile to SPIR-V on any host
            return true;
        case GxShaderTarget::MetalIr:
#if defined(__APPLE__)
            return true;
#else
            return false;
#endif
    }
    return false;
}

} // namespace psynder::asset
