// SPDX-License-Identifier: MIT
// Psynder - job system public facade. Lane 04. The Chase-Lev work-stealing
// scheduler, the stackful-fiber execution backend, and the heterogeneous P/E
// worker pools all live in the internal Scheduler / Fiber / Topology /
// ChaseLevDeque units; this file only forwards the FROZEN public API
// (JobSystem.h) to them.
//
// Drop-in compatibility: when the pool has NOT been started, every operation
// runs synchronously on the caller's thread, exactly as the original Phase-0
// scaffold did. Engine code and tests that submit jobs without ever calling
// start() therefore keep working unchanged; calling start() upgrades the same
// calls to real multi-threaded execution.

#include "JobSystem.h"

#include "Scheduler.h"

#include <atomic>

namespace psynder::jobs {

namespace {
// Monotonic id source for the synchronous (not-started) path, so callers still
// receive a unique, valid-looking handle.
std::atomic<u32> g_sync_id{1};
}  // namespace

JobSystem& JobSystem::Get() {
    static JobSystem s;
    return s;
}

void JobSystem::start(u32 worker_count) { detail::sched_start(worker_count); }

void JobSystem::stop() { detail::sched_stop(); }

JobHandle JobSystem::submit(const JobDesc& desc, JobHandle dep) {
    if (detail::sched_running()) {
        return detail::sched_submit(desc, dep);
    }
    // Synchronous fallback: run inline, honouring the dependency trivially
    // (it has already completed in this single-threaded path).
    if (desc.fn) {
        desc.fn(desc.user);
    }
    return JobHandle{g_sync_id.fetch_add(1, std::memory_order_relaxed), 0};
}

void JobSystem::wait(JobHandle h) {
    if (detail::sched_running()) {
        detail::sched_wait(h);
    }
    // Otherwise the job already ran synchronously at submit() time.
}

void JobSystem::parallel_for(usize begin, usize end, usize grain,
                             const std::function<void(usize, usize)>& body) {
    if (detail::sched_running()) {
        detail::sched_parallel_for(begin, end, grain, body);
        return;
    }
    if (begin < end && body) {
        body(begin, end);
    }
}

u32 JobSystem::worker_count() const noexcept {
    return detail::sched_running() ? detail::sched_worker_count() : 1u;
}

u32 JobSystem::current_worker() const noexcept {
    return detail::sched_running() ? detail::sched_current_worker() : ~0u;
}

}  // namespace psynder::jobs
