// SPDX-License-Identifier: MIT
// Psynder - stackful fiber primitive (lane 04, internal).
//
// A thin, allocation-light wrapper over the platform context-switch APIs used
// by the JobSystem's fiber execution backend (see Scheduler.cpp):
//   - Windows: ConvertThreadToFiber / CreateFiber / SwitchToFiber.
//   - POSIX:   getcontext / makecontext / swapcontext (ucontext).
//
// A fiber is a saved CPU context + its own stack. Switching to a fiber resumes
// it exactly where it last switched away, on whatever thread performs the
// switch (fibers may migrate between worker threads). The entry function MUST
// NOT return: it runs the scheduler's per-fiber loop and switches back to the
// owning worker's scheduler fiber when the job completes or yields.
//
// PSY_JOBS_HAVE_FIBERS is 1 when a context-switch backend is available, 0
// otherwise. The scheduler falls back to its thread (wait-helping) backend
// when fibers are unavailable.

#pragma once

#include "core/Types.h"

#if defined(_WIN32)
#  define PSY_JOBS_HAVE_FIBERS 1
#elif defined(__has_include)
#  if __has_include(<ucontext.h>)
#    define PSY_JOBS_HAVE_FIBERS 1
#  else
#    define PSY_JOBS_HAVE_FIBERS 0
#  endif
#else
#  define PSY_JOBS_HAVE_FIBERS 0
#endif

namespace psynder::jobs::detail {

// Opaque platform fiber. Defined in Fiber.cpp.
struct Fiber;

using FiberEntry = void (*)(void* arg);

// Promote the calling thread to a fiber context (its "scheduler fiber").
// Returns nullptr if fibers are unavailable. Pair with fiber_release_thread().
Fiber* fiber_for_thread();
void fiber_release_thread(Fiber* thread_fiber);

// Create a stackful fiber that runs `entry(arg)` the first time it is switched
// to. `entry` must never return. Returns nullptr on failure / no fiber support.
Fiber* fiber_create(usize stack_bytes, FiberEntry entry, void* arg);
void fiber_destroy(Fiber* f);

// Save the current context into `from` and resume `to`. Both must be non-null
// and obtained from fiber_for_thread()/fiber_create() on this process.
void fiber_switch(Fiber* from, Fiber* to);

}  // namespace psynder::jobs::detail
