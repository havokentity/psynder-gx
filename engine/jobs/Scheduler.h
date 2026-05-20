// SPDX-License-Identifier: MIT
// Psynder - JobSystem scheduler internals (lane 04, internal header).
//
// This is the implementation surface behind the FROZEN public JobSystem API
// (JobSystem.h). It exposes the internal execution-backend switch (thread vs
// stackful-fiber) and the free functions the JobSystem facade forwards to.
// Other lanes must NOT include this header - they code against JobSystem.h.
// Unit tests in this lane may include it to flip the backend switch and to
// assert backend availability.

#pragma once

#include "JobSystem.h"
#include "core/Types.h"

#include <functional>

namespace psynder::jobs::detail {

// Execution backend for nested wait()/await:
//   Thread - the waiting worker keeps its OS thread busy by running other
//            ready jobs inline (wait-helping). Always available.
//   Fiber  - each job runs on its own stackful fiber; a wait() suspends the
//            fiber and frees the worker to run other fibers, with no stack
//            growth. Available where PSY_JOBS_HAVE_FIBERS (Win32 / ucontext).
// Both backends satisfy the contract "a job can await a dependency without
// blocking its worker thread"; the fiber backend additionally bounds stack use.
enum class Backend : u8 { Thread, Fiber };

// True if the stackful-fiber backend is compiled in for this platform.
bool fibers_supported() noexcept;

// Internal switch selecting the backend used by the NEXT sched_start(). The
// default is Backend::Thread. Requesting Backend::Fiber when fibers are
// unavailable silently keeps Thread. The env var PSY_JOBS_BACKEND
// (thread|fiber) overrides this at start() time.
void set_preferred_backend(Backend b) noexcept;
Backend preferred_backend() noexcept;

// Lifecycle + operations backing the public JobSystem facade. All of these are
// only invoked while the scheduler is running; the facade handles the
// not-running synchronous fallback itself.
void sched_start(u32 worker_count);
void sched_stop();
bool sched_running() noexcept;
Backend sched_active_backend() noexcept;
u32 sched_worker_count() noexcept;
u32 sched_current_worker() noexcept;

JobHandle sched_submit(const JobDesc& desc, JobHandle dep);
void sched_wait(JobHandle h);
void sched_parallel_for(usize begin, usize end, usize grain,
                        const std::function<void(usize, usize)>& body);

}  // namespace psynder::jobs::detail
