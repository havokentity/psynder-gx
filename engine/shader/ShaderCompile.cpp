// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/ShaderCompile.cpp
//
// Lane 08 — Slang compiler integration.
//
// Design points (DESIGN §2.2 / §7.7 / §10.7):
//  - Primary: invoke the installed `slangc` binary for SPIR-V and Metal IR.
//  - Secondary fallback: glslc + spirv-cross if slangc is absent (not
//    exercised here; slangc 2026.1 is present on the build host).
//  - compile_to_spirv / compile_to_metal_ir: shell out to slangc with the
//    appropriate -profile and -target flags.
//  - Hot-reload (hot_reload_changed): poll source file mtimes, recompile
//    changed entries, atomically swap PipelineHandle registrations.
//    A native file-watcher (kqueue / inotify / ReadDirectoryChanges) replaces
//    the poll in M2 per DESIGN §14 (platform-native watchers).

#include "shader/PublicShader.h"
#include <utility>
#include "shader/impl/PipelineCreateBackend.h"
#include "shader/impl/ShaderImpl.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

// Platform: pipe() / popen() for capturing slangc stdout/stderr
#if defined(_WIN32)
#  include <windows.h>   // NOLINT
#else
#  include <unistd.h>
#endif

namespace psynder::shader {

// ─── Utilities ────────────────────────────────────────────────────────────

namespace {

// Run a shell command, capturing combined stdout+stderr.
// Returns the process exit code; fills out with captured text.
int run_command(const std::string& cmd, std::string& out)
{
    out.clear();
#if defined(_WIN32)
    FILE* fp = _popen(cmd.c_str(), "r");
#else
    FILE* fp = ::popen(cmd.c_str(), "r");
#endif
    if (!fp) { out = "(popen failed)"; return -1; }

    char buf[512];
    while (::fgets(buf, sizeof(buf), fp)) {
        out += buf;
    }
#if defined(_WIN32)
    return _pclose(fp);
#else
    return ::pclose(fp);
#endif
}

// Read all bytes of a binary file into a vector.
bool read_binary(const char* path, std::vector<uint8_t>& out)
{
    FILE* f = ::fopen(path, "rb");
    if (!f) return false;
    ::fseek(f, 0, SEEK_END);
    long sz = ::ftell(f);
    ::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { ::fclose(f); return false; }
    out.resize(static_cast<size_t>(sz));
    bool ok = (::fread(out.data(), 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz));
    ::fclose(f);
    return ok;
}

// Stage → slangc stage name string
const char* stage_name_slang(Stage s)
{
    switch (s) {
        case Stage::Vertex:       return "vertex";
        case Stage::Fragment:     return "fragment";
        case Stage::Compute:      return "compute";
        case Stage::Mesh:         return "mesh";
        case Stage::Task:         return "amplification";
        case Stage::RayGen:       return "raygeneration";
        case Stage::RayMiss:      return "miss";
        case Stage::RayHitClosest:return "closesthit";
        case Stage::RayHitAny:    return "anyhit";
    }
    return "vertex";
}

// Build a unique tmp filename in /tmp for the compiled output blob.
std::string tmp_path(const char* slang_path, const char* entry, const char* ext)
{
    // Simple hash — collisions are fine here; we clean up after each build.
    std::size_t h = std::hash<std::string>{}(std::string(slang_path) + entry);
    char buf[256];
    ::snprintf(buf, sizeof(buf), "/tmp/psynder_gx_shader_%08zx_%s%s",
               h & 0xFFFFFFFF, entry, ext);
    return buf;
}

} // anonymous namespace

// ─── impl helpers (definitions) ──────────────────────────────────────────

namespace impl {

std::int64_t file_mtime(const char* path)
{
    struct ::stat st;
    if (::stat(path, &st) != 0) return -1;
#if defined(__APPLE__) || defined(__linux__)
    return static_cast<std::int64_t>(st.st_mtime);
#else
    return static_cast<std::int64_t>(st.st_mtime);
#endif
}

bool compile_to_spirv(
    const char*                slang_path,
    const char*                entry_point,
    Stage                      stage,
    std::vector<std::uint8_t>& out_spirv,
    std::string*               out_log)
{
    const std::string out_file = tmp_path(slang_path, entry_point, ".spv");

    // slangc -entry <ep> -stage <stage> -profile glsl_450 -target spirv-asm
    // Use -target spirv (binary SPIR-V words, not text asm)
    std::string cmd;
    cmd  = "slangc ";
    cmd += "\"";
    cmd += slang_path;
    cmd += "\" ";
    cmd += "-entry ";     cmd += entry_point;  cmd += " ";
    cmd += "-stage ";     cmd += stage_name_slang(stage); cmd += " ";
    cmd += "-profile glsl_450 ";
    cmd += "-target spirv ";
    // Keep the SPIR-V OpEntryPoint named after the slang entry (e.g. "vs_main")
    // rather than slang's default rename to "main", so it matches the pName
    // lane 08 passes to vkCreateGraphicsPipelines (GraphicsPipelineDesc
    // .entry_point_vs/fs). Without this the PSO build fails with
    // VUID-VkPipelineShaderStageCreateInfo-pName-00707 ("entry point not found").
    cmd += "-fvk-use-entrypoint-name ";
    cmd += "-o \""; cmd += out_file; cmd += "\" 2>&1";

    std::string log_out;
    int rc = run_command(cmd, log_out);
    if (out_log) *out_log = log_out;

    if (rc != 0) return false;
    return read_binary(out_file.c_str(), out_spirv);
}

bool compile_to_metal_ir(
    const char*                slang_path,
    const char*                entry_point,
    Stage                      stage,
    std::vector<std::uint8_t>& out_metal_ir,
    std::string*               out_log)
{
    // Metal IR (metallib) is macOS-only and requires the Apple xcrun / metal
    // toolchain on top of slangc's Metal source output.
    // Wave B: slangc -target metallib is only supported with full Xcode SDK.
    // We emit Metal source first via -target metal, then invoke xcrun -sdk
    // macosx metal to compile to .ir / .metallib.
#if !defined(__APPLE__)
    if (out_log) *out_log = "stub: Metal IR compilation only supported on macOS";
    (void)slang_path; (void)entry_point; (void)stage; (void)out_metal_ir;
    return false;
#else
    const std::string metal_src  = tmp_path(slang_path, entry_point, ".metal");
    const std::string metal_ir   = tmp_path(slang_path, entry_point, ".metallib");

    // Step 1: slangc -> Metal source. Slang 2026.x accepts `metal` as a
    // target, but `metallib` is a target, not a valid profile name.
    std::string cmd1;
    cmd1  = "slangc ";
    cmd1 += "\""; cmd1 += slang_path; cmd1 += "\" ";
    cmd1 += "-entry "; cmd1 += entry_point; cmd1 += " ";
    cmd1 += "-stage "; cmd1 += stage_name_slang(stage); cmd1 += " ";
    cmd1 += "-target metal ";
    cmd1 += "-o \""; cmd1 += metal_src; cmd1 += "\" 2>&1";

    std::string log1;
    if (run_command(cmd1, log1) != 0) {
        if (out_log) *out_log = log1;
        return false;
    }

    // Step 2: xcrun metal → .air → .metallib
    const std::string air_path = tmp_path(slang_path, entry_point, ".air");
    std::string cmd2;
    cmd2  = "xcrun -sdk macosx metal -c ";
    cmd2 += "\""; cmd2 += metal_src; cmd2 += "\" ";
    cmd2 += "-o \""; cmd2 += air_path; cmd2 += "\" 2>&1";

    std::string log2;
    if (run_command(cmd2, log2) != 0) {
        if (out_log) *out_log = log2;
        return false;
    }

    std::string cmd3;
    cmd3  = "xcrun -sdk macosx metallib ";
    cmd3 += "\""; cmd3 += air_path; cmd3 += "\" ";
    cmd3 += "-o \""; cmd3 += metal_ir; cmd3 += "\" 2>&1";

    std::string log3;
    if (run_command(cmd3, log3) != 0) {
        if (out_log) *out_log = log3;
        return false;
    }

    if (out_log) *out_log = "(ok)";
    return read_binary(metal_ir.c_str(), out_metal_ir);
#endif
}

} // namespace impl

// ─── Backend-specific stage-blob compile ─────────────────────────────────
//
// On PSYNDER_GX_BACKEND_VULKAN builds: compile to SPIR-V via slangc.
// On PSYNDER_GX_BACKEND_METAL builds:  compile to Metal IR via slangc +
//                                      xcrun metal + xcrun metallib.
// On builds with neither define: only SPIR-V (host-tools / asset cook only).
//
// The compiled bytes are stored unchanged in CachedPipeline::spirv_*; the
// member is misnamed for historical reasons but holds whichever blob
// format the active backend consumes.
namespace {

bool compile_stage_for_active_backend(
    const char*               slang_path,
    const char*               entry,
    Stage                     stage,
    std::vector<std::uint8_t>& out_blob,
    std::string*              out_log)
{
#if defined(PSYNDER_GX_BACKEND_METAL)
    return psynder::shader::impl::compile_to_metal_ir(
        slang_path, entry, stage, out_blob, out_log);
#else
    // Vulkan build (or no-backend test build) — SPIR-V.
    return psynder::shader::impl::compile_to_spirv(
        slang_path, entry, stage, out_blob, out_log);
#endif
}

// Register the watch path for hot-reload polling. Caller must hold reg.mu.
void touch_watch_locked(impl::PipelineRegistry& reg,
                        const char*             path,
                        std::int64_t            mtime)
{
    for (auto& w : reg.watch_list) {
        if (w.path == path) return;
    }
    reg.watch_list.push_back({path, mtime});
}

} // anonymous namespace

// ─── Public API implementations ──────────────────────────────────────────

PipelineHandle create_graphics(const GraphicsPipelineDesc& desc)
{
    if (!desc.slang_path) return PipelineHandle{};

    const char* vs_ep = desc.entry_point_vs ? desc.entry_point_vs : "vs_main";
    const char* fs_ep = desc.entry_point_fs ? desc.entry_point_fs : "fs_main";

    auto& reg = impl::PipelineRegistry::get();

    impl::CachedPipeline entry;
    entry.slang_path   = desc.slang_path;
    entry.entry_vs     = vs_ep;
    entry.entry_fs     = fs_ep;
    entry.vertex_input = desc.vertex_input;  // cached for hot-reload re-PSO
    entry.color_format_count = desc.color_format_count;
    entry.depth_format = desc.depth_format;
    entry.enable_depth_write = desc.enable_depth_write;
    entry.enable_blend = desc.enable_blend;
    entry.fill_wireframe = (desc.fill_mode == FillMode::Wireframe);

    std::string log_vs, log_fs;
    bool vs_ok = compile_stage_for_active_backend(
        desc.slang_path, vs_ep, Stage::Vertex, entry.spirv_vs, &log_vs);
    bool fs_ok = compile_stage_for_active_backend(
        desc.slang_path, fs_ep, Stage::Fragment, entry.spirv_fs, &log_fs);

    if (!vs_ok || !fs_ok) {
        // Diagnostics surfaced to the engine console (lane 01 wires this up).
        // For Wave B: print to stderr as fallback.
        if (!vs_ok) ::fprintf(stderr, "[shader] VS compile failed: %s\n  %s\n",
                              desc.slang_path, log_vs.c_str());
        if (!fs_ok) ::fprintf(stderr, "[shader] FS compile failed: %s\n  %s\n",
                              desc.slang_path, log_fs.c_str());
        return PipelineHandle{};
    }

    // Atomically allocate the handle id so concurrent callers from
    // different worker threads don't collide (M1+ multi-threaded init).
    entry.id           = reg.allocate_id();
    entry.source_mtime = impl::file_mtime(desc.slang_path);

    // Construct the API-native PSO + register with lane 07. The call must
    // happen OUTSIDE reg.mu because the backend implementations grab their
    // own mutex (e.g. Vulkan's g_ctx_mu) and we never want nested locking.
    //
    // desc.vertex_input is forwarded by-value-via-const-ref. attr_count==0
    // (the default-constructed sentinel) triggers the DefaultVertexLayout
    // fallback inside the backend — preserves every pre-lane-09 callsite.
    const bool pso_ok = impl::create_and_register_graphics_pso(
        entry.id, entry.spirv_vs, entry.spirv_fs, vs_ep, fs_ep,
        desc.vertex_input, desc.color_format_count, desc.depth_format,
        desc.enable_depth_write, desc.enable_blend, entry.fill_wireframe);
    entry.pso_registered = pso_ok;
    if (!pso_ok) {
        // The handle id is still valid; lane 07's bind_pipeline path will
        // emit its "not registered" log + render-blank fallback. We don't
        // want to fail the create_graphics call here because the caller
        // (render lane) may want to keep the handle around to retry on
        // hot reload.
        ::fprintf(stderr,
            "[shader] graphics PSO creation failed for id=%u (path=%s) — "
            "lane 07 bind_pipeline will report 'not registered'\n",
            entry.id, desc.slang_path);
    }

    const auto id = entry.id;
    {
        std::lock_guard<std::mutex> lock(reg.mu);
        touch_watch_locked(reg, desc.slang_path, entry.source_mtime);
        reg.pipelines.emplace(id, std::move(entry));
    }
    return PipelineHandle{id};
}

PipelineHandle create_compute(const ComputePipelineDesc& desc)
{
    if (!desc.slang_path) return PipelineHandle{};

    const char* cs_ep = desc.entry_point_cs ? desc.entry_point_cs : "cs_main";

    auto& reg = impl::PipelineRegistry::get();

    impl::CachedPipeline entry;
    entry.slang_path = desc.slang_path;
    entry.entry_cs   = cs_ep;

    std::string log_cs;
    bool ok = compile_stage_for_active_backend(
        desc.slang_path, cs_ep, Stage::Compute, entry.spirv_cs, &log_cs);
    if (!ok) {
        ::fprintf(stderr, "[shader] CS compile failed: %s\n  %s\n",
                  desc.slang_path, log_cs.c_str());
        return PipelineHandle{};
    }

    entry.id           = reg.allocate_id();
    entry.source_mtime = impl::file_mtime(desc.slang_path);

    const bool pso_ok = impl::create_and_register_compute_pso(
        entry.id, entry.spirv_cs, cs_ep);
    entry.pso_registered = pso_ok;
    if (!pso_ok) {
        ::fprintf(stderr,
            "[shader] compute PSO creation failed for id=%u (path=%s) — "
            "lane 07 dispatch will report 'not registered'\n",
            entry.id, desc.slang_path);
    }

    const auto id = entry.id;
    {
        std::lock_guard<std::mutex> lock(reg.mu);
        touch_watch_locked(reg, desc.slang_path, entry.source_mtime);
        reg.pipelines.emplace(id, std::move(entry));
    }
    return PipelineHandle{id};
}

PipelineHandle create_rt(const RtPipelineDesc& desc)
{
    // RT pipeline: M5 implementation. Stub returns invalid handle.
    // The shader registry records the request so the RT stub lane (10) can
    // see what shaders have been requested at M5 integration.
    (void)desc;
    return PipelineHandle{};  // stub
}

void destroy_pipeline(PipelineHandle h)
{
    if (!h.valid()) return;
    auto& reg = impl::PipelineRegistry::get();
    {
        std::lock_guard<std::mutex> lock(reg.mu);
        reg.pipelines.erase(h.id);
    }
    // M1: API resources (VkPipeline / MTLRenderPipelineState) are leaked
    // by design — see PipelineCreateBackend.h. Real retire-with-
    // frames-in-flight integrates with lane 07's reaper in M2.
    impl::release_registered_pso(h.id);
}

void hot_reload_changed()
{
    // Wave B: mtime-poll hot-reload.
    // Iterate the watch list; recompile anything whose mtime has advanced.
    // The atomic pipeline swap (old handle remains valid for in-flight frames)
    // is deferred to M2 when lane 09 provides the pipeline-swap protocol.
    // For now we recompile the blobs in-place AND rebuild the API-native
    // PSO so lane 07's bind_pipeline picks up the new shader code on the
    // next bind. The previous PSO is leaked (M1) — M2 wires in the
    // frames-in-flight reaper.
    //
    // M2 upgrade path: replace this function body with a platform-native
    // file-watcher subscription (kqueue on macOS, inotify on Linux,
    // ReadDirectoryChangesW on Win32) so the OS notifies us instead of polling.

    auto& reg = impl::PipelineRegistry::get();

    // Snapshot the watch list under lock so we can iterate without holding
    // reg.mu through compile (compile_to_* spawns slangc, which is slow).
    struct WatchSnapshot { std::string path; std::int64_t prev_mtime; };
    std::vector<WatchSnapshot> snap;
    {
        std::lock_guard<std::mutex> lock(reg.mu);
        snap.reserve(reg.watch_list.size());
        for (auto& w : reg.watch_list) snap.push_back({w.path, w.last_mtime});
    }

    for (auto& w : snap) {
        const std::int64_t cur_mtime = impl::file_mtime(w.path.c_str());
        if (cur_mtime <= 0 || cur_mtime <= w.prev_mtime) continue;

        // Update the watch mtime under lock.
        {
            std::lock_guard<std::mutex> lock(reg.mu);
            for (auto& wl : reg.watch_list) {
                if (wl.path == w.path) { wl.last_mtime = cur_mtime; break; }
            }
        }

        // Collect the pipeline ids that reference this source file. We
        // snapshot ids + entries under the lock, recompile outside, then
        // re-publish blobs back into the map under the lock.
        struct PipeSnap {
            std::uint32_t   id;
            std::string     entry_vs, entry_fs, entry_cs;
            VertexInputDesc vertex_input;
            std::uint32_t   color_format_count;
            std::uint8_t    depth_format;
            bool            enable_depth_write;
            bool            enable_blend;
            bool            fill_wireframe;
        };
        std::vector<PipeSnap> targets;
        {
            std::lock_guard<std::mutex> lock(reg.mu);
            for (auto& [id, cached] : reg.pipelines) {
                if (cached.slang_path != w.path) continue;
                targets.push_back({id,
                                   cached.entry_vs,
                                   cached.entry_fs,
                                   cached.entry_cs,
                                   cached.vertex_input,
                                   cached.color_format_count,
                                   cached.depth_format,
                                   cached.enable_depth_write,
                                   cached.enable_blend,
                                   cached.fill_wireframe});
            }
        }

        for (auto& t : targets) {
            std::string log;
            std::vector<std::uint8_t> new_vs, new_fs, new_cs;
            bool any_gfx = false, gfx_ok = true;

            if (!t.entry_vs.empty()) {
                any_gfx = true;
                if (!compile_stage_for_active_backend(
                        w.path.c_str(), t.entry_vs.c_str(),
                        Stage::Vertex, new_vs, &log)) {
                    ::fprintf(stderr, "[shader] hot-reload VS failed: %s\n", log.c_str());
                    gfx_ok = false;
                }
            }
            if (!t.entry_fs.empty()) {
                any_gfx = true;
                if (!compile_stage_for_active_backend(
                        w.path.c_str(), t.entry_fs.c_str(),
                        Stage::Fragment, new_fs, &log)) {
                    ::fprintf(stderr, "[shader] hot-reload FS failed: %s\n", log.c_str());
                    gfx_ok = false;
                }
            }
            if (!t.entry_cs.empty()) {
                if (!compile_stage_for_active_backend(
                        w.path.c_str(), t.entry_cs.c_str(),
                        Stage::Compute, new_cs, &log)) {
                    ::fprintf(stderr, "[shader] hot-reload CS failed: %s\n", log.c_str());
                }
            }

            // Re-register PSO with lane 07 (overwrites the existing entry
            // in the registration shim's table — see vk_pipeline_shim::find
            // and pipeline_shim::find which both overwrite on duplicate id).
            // We pass the freshly-compiled blobs; the older PSO leaks as
            // documented above.
            if (any_gfx && gfx_ok) {
                impl::create_and_register_graphics_pso(
                    t.id, new_vs, new_fs,
                    t.entry_vs.c_str(), t.entry_fs.c_str(),
                    t.vertex_input, t.color_format_count, t.depth_format,
                    t.enable_depth_write, t.enable_blend, t.fill_wireframe);
            }
            if (!t.entry_cs.empty() && !new_cs.empty()) {
                impl::create_and_register_compute_pso(
                    t.id, new_cs, t.entry_cs.c_str());
            }

            // Publish the new blobs back to the cache.
            {
                std::lock_guard<std::mutex> lock(reg.mu);
                auto it = reg.pipelines.find(t.id);
                if (it == reg.pipelines.end()) continue;
                if (!new_vs.empty()) it->second.spirv_vs = std::move(new_vs);
                if (!new_fs.empty()) it->second.spirv_fs = std::move(new_fs);
                if (!new_cs.empty()) it->second.spirv_cs = std::move(new_cs);
                it->second.source_mtime = cur_mtime;
            }
        }
    }
}

} // namespace psynder::shader
