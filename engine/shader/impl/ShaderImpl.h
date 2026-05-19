// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/impl/ShaderImpl.h
//
// Internal implementation types for lane 08 — NOT part of the public API.
// Only included by shader/*.cpp files. Do NOT #include from other lanes.

#pragma once

#include "shader/PublicShader.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace psynder::shader::impl {

// ─── Pipeline cache entry ─────────────────────────────────────────────────
struct CachedPipeline {
    std::uint32_t        id          = 0;
    std::string          slang_path;
    std::string          entry_vs;
    std::string          entry_fs;
    std::string          entry_cs;
    // Compiled SPIR-V or Metal IR blobs (one per stage)
    std::vector<uint8_t> spirv_vs;
    std::vector<uint8_t> spirv_fs;
    std::vector<uint8_t> spirv_cs;
    // mtime snapshot used by the hot-reload watcher
    std::int64_t         source_mtime = 0;
};

// ─── Registry (global singleton for this translation unit) ────────────────
struct PipelineRegistry {
    std::unordered_map<std::uint32_t, CachedPipeline> pipelines;
    std::uint32_t                                      next_id = 1;

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
