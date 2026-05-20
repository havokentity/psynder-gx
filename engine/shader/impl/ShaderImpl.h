// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/impl/ShaderImpl.h
//
// Internal implementation types for lane 08 — NOT part of the public API.
// Only included by shader/*.cpp files. Do NOT #include from other lanes.

#pragma once

#include "shader/PublicShader.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace psynder::shader::impl {

// ─── Pipeline cache entry ─────────────────────────────────────────────────
//
// The cache holds the COMPILED-IR blobs (SPIR-V on Vulkan builds, .metallib
// bytes on Metal builds — populated by ShaderCompile.cpp's compile_to_spirv
// or compile_to_metal_ir respectively). The blobs are kept for hot reload
// (so we can diff against a freshly-compiled blob and skip re-PSO if
// unchanged) and for debug introspection.
//
// The actual VkPipeline / MTLRenderPipelineState lives in lane 07's
// per-backend pipeline shim (see VulkanBackend.cpp / MetalBackend.mm).
// Lane 08 forwards the PSO via psynder_gx_{vk,mtl}_register_pipeline.
struct CachedPipeline {
    std::uint32_t        id          = 0;
    std::string          slang_path;
    std::string          entry_vs;
    std::string          entry_fs;
    std::string          entry_cs;
    // Compiled blobs. Members reused for both backends — "spirv_*" name
    // is historical; the bytes are SPIR-V on PSYNDER_GX_BACKEND_VULKAN
    // builds and Metal IR (.metallib) on PSYNDER_GX_BACKEND_METAL builds.
    std::vector<uint8_t> spirv_vs;
    std::vector<uint8_t> spirv_fs;
    std::vector<uint8_t> spirv_cs;
    // mtime snapshot used by the hot-reload watcher
    std::int64_t         source_mtime = 0;
    // True once the PSO has been constructed + handed off to lane 07's
    // registration shim. On hot-reload we recompile + re-register.
    bool                 pso_registered = false;
};

// ─── Registry (global singleton for this translation unit) ────────────────
//
// Pipeline ids are allocated from a monotonic atomic counter starting at 1
// (id=0 is reserved for the sentinel "invalid" value PipelineHandle{}).
// The counter is process-wide so render lanes can race on create_graphics
// from different worker threads without colliding on id assignment.
//
// The `pipelines` map + `watch_list` are guarded by `mu` because both
// create_graphics and hot_reload_changed mutate them, and the hot-reload
// path can be invoked from a non-render worker thread.
struct PipelineRegistry {
    std::unordered_map<std::uint32_t, CachedPipeline> pipelines;
    std::atomic<std::uint32_t>                        next_id { 1 };
    std::mutex                                        mu;

    // mtime-poll hot-reload state: list of (path, last mtime) watched
    struct WatchEntry {
        std::string  path;
        std::int64_t last_mtime = 0;
    };
    std::vector<WatchEntry> watch_list;

    static PipelineRegistry& get() {
        static PipelineRegistry inst;
        return inst;
    }

    std::uint32_t allocate_id() {
        return next_id.fetch_add(1, std::memory_order_relaxed);
    }

private:
    PipelineRegistry() = default;
};

// ─── Slang compile helpers (declarations) ────────────────────────────────
// Compile a single .slang file for one entry point to SPIR-V using the
// installed slangc binary. Returns true on success; fills out_spirv.
// Writes diagnostic text to out_log if not nullptr.
bool compile_to_spirv(
    const char*               slang_path,
    const char*               entry_point,
    Stage                     stage,
    std::vector<std::uint8_t>& out_spirv,
    std::string*               out_log = nullptr);

// Compile to Metal IR (macOS + Xcode toolchain only).
bool compile_to_metal_ir(
    const char*               slang_path,
    const char*               entry_point,
    Stage                     stage,
    std::vector<std::uint8_t>& out_metal_ir,
    std::string*               out_log = nullptr);

// Returns the wall-clock mtime of a file (seconds since epoch), or -1 if
// the file does not exist.
std::int64_t file_mtime(const char* path);

// Resolve a path that might be relative to the engine source root.
// For Wave B we simply forward it as-is; a proper VFS lookup is M2+.
inline const char* resolve_path(const char* p) { return p; }

} // namespace psynder::shader::impl
