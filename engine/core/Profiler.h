// SPDX-License-Identifier: MIT
// Psynder — engine-wide profiling policy: canonical Tracy zone names, zone
// colors, the per-frame profiler hook, and the allocator-plot helper.
//
// Tracy.h is the raw macro shim (real macros vs no-ops). This header is the
// shared *vocabulary* every lane profiles against: identical zone-name string
// literals + colors mean a Tracy capture taken from any subsystem lines up
// across the whole engine. The hot subsystems mark their critical sections
// with the canonical names below, so one capture shows frame / jobs / render
// / physics / audio side by side instead of a pile of ad-hoc labels.
//
// Usage in a hot subsystem (owned by another lane):
//   #include "core/Profiler.h"
//   void JobSystem::execute(Job& j) {
//       PSY_ZONE_C(PSY_PROF_JOBS_EXECUTE, psynder::prof::kColorJobs);
//       ...
//   }
// And once per rendered frame, from the engine main loop after present:
//   psynder::mem::Heatmap::get().plot_to_tracy();  // per-tag allocator plots
//   psynder::prof::end_frame();                    // Tracy frame boundary

#pragma once

#include "Tracy.h"
#include "Types.h"

// ─── Canonical zone names ──────────────────────────────────────────────────
// Kept as string-literal macros so they're usable as PSY_ZONE arguments
// without question (Tracy stores the pointer, so it must have static storage)
// and so the dotted hierarchy reads naturally in the Tracy zone tree.
#define PSY_PROF_FRAME         "frame"
#define PSY_PROF_JOBS_EXECUTE  "jobs.execute"
#define PSY_PROF_RENDER_SUBMIT "render.submit"
#define PSY_PROF_PHYSICS_TICK  "physics.tick"
#define PSY_PROF_AUDIO_MIX     "audio.mix"

// Canonical Tracy plot name for the rolled-up live-bytes total (see
// plot_memory()). Per-tag plot names are owned by the allocator heatmap.
#define PSY_PROF_PLOT_MEM_TOTAL "mem.live.total"

namespace psynder::prof {

// True when the build was compiled with Tracy enabled. Lets a call site take
// a cheap runtime branch (e.g. skip building a debug string it would only
// hand to PSY_ZONE_TEXT) without threading the preprocessor define through
// its own code.
#if defined(PSYNDER_ENABLE_TRACY) && PSYNDER_ENABLE_TRACY
inline constexpr bool kEnabled = true;
#else
inline constexpr bool kEnabled = false;
#endif

// 0xRRGGBB zone colors, one per hot subsystem, so the flame graph stays
// readable at a glance regardless of which lane emitted the zone.
inline constexpr u32 kColorFrame   = 0x4C78A8u;  // steel blue
inline constexpr u32 kColorJobs    = 0xC88A2Cu;  // amber
inline constexpr u32 kColorRender  = 0x4E9A4Eu;  // green
inline constexpr u32 kColorPhysics = 0xB04A4Au;  // brick red
inline constexpr u32 kColorAudio   = 0x8A5CC8u;  // violet
inline constexpr u32 kColorMemory  = 0x9A9A5Cu;  // olive

// Mark the end of one rendered frame: advance the engine frame counter and
// emit the Tracy frame boundary. Call exactly once per presented frame from
// the main loop, after present. Returns the just-completed frame index. The
// counter advances even when Tracy is compiled out (the flight recorder
// stamps events with it regardless).
u64 end_frame() noexcept;

// Monotonic count of frames completed via end_frame(); 0 until the first one.
u64 current_frame() noexcept;

// Name the calling thread in the Tracy capture (no-op when Tracy is off).
// Call once at thread startup; `name` must outlive the call (a literal).
void set_thread_name(const char* name) noexcept;

// Feed the rolled-up live-byte total across every allocator tag to Tracy as a
// single named plot. Cheap; safe to call once per frame. No-op when Tracy is
// off. For the full per-tag/per-arena breakdown, use mem::Heatmap.
void plot_memory() noexcept;

}  // namespace psynder::prof
