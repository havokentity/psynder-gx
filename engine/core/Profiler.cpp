// SPDX-License-Identifier: MIT
// Psynder — profiler hook impl. The Tracy-touching code lives here so callers
// of Profiler.h don't drag <tracy/Tracy.hpp> into every TU that just wants a
// zone name or the frame counter.

#include "Profiler.h"

#include "alloc/Allocator.h"

#include <atomic>

namespace psynder::prof {

namespace {

// Process-global frame counter. Relaxed is fine: the value is advisory (frame
// stamping + Tracy), never used to order memory.
std::atomic<u64> g_frame{0};

}  // namespace

u64 end_frame() noexcept {
    const u64 completed = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    PSY_ZONE_FRAME();
    return completed;
}

u64 current_frame() noexcept {
    return g_frame.load(std::memory_order_relaxed);
}

void set_thread_name(const char* name) noexcept {
    if (!name) return;
    PSY_ZONE_THREAD(name);
}

void plot_memory() noexcept {
#if defined(PSYNDER_ENABLE_TRACY) && PSYNDER_ENABLE_TRACY
    usize total = 0;
    for (u32 i = 0; i < static_cast<u32>(mem::Tag::Count); ++i) {
        total += mem::current_usage(static_cast<mem::Tag>(i));
    }
    PSY_ZONE_PLOT(PSY_PROF_PLOT_MEM_TOTAL, static_cast<double>(total));
#endif
}

}  // namespace psynder::prof
