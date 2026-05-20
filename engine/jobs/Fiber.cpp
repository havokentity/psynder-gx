// SPDX-License-Identifier: MIT
// Psynder - stackful fiber primitive implementation. See Fiber.h.

// POSIX: expose the ucontext API (glibc hides it behind _XOPEN_SOURCE) while
// keeping the Darwin BSD extensions visible. Must precede every include.
#if !defined(_WIN32)
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 700
#  endif
#  if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#    define _DARWIN_C_SOURCE 1
#  endif
#endif

#include "Fiber.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif PSY_JOBS_HAVE_FIBERS
#  include <cstdint>
#  include <cstdlib>
#  include <ucontext.h>
#endif

namespace psynder::jobs::detail {

#if defined(_WIN32)

struct Fiber {
    void* handle = nullptr;
    FiberEntry entry = nullptr;
    void* arg = nullptr;
    bool is_thread = false;
};

namespace {
void CALLBACK win_trampoline(void* param) {
    auto* f = static_cast<Fiber*>(param);
    f->entry(f->arg);  // contract: never returns
}
}  // namespace

Fiber* fiber_for_thread() {
    auto* f = new Fiber{};
    f->is_thread = true;
    f->handle = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
    if (!f->handle) {
        delete f;
        return nullptr;
    }
    return f;
}

void fiber_release_thread(Fiber* thread_fiber) {
    if (!thread_fiber) {
        return;
    }
    if (thread_fiber->is_thread && IsThreadAFiber()) {
        ConvertFiberToThread();
    }
    delete thread_fiber;
}

Fiber* fiber_create(usize stack_bytes, FiberEntry entry, void* arg) {
    auto* f = new Fiber{};
    f->entry = entry;
    f->arg = arg;
    f->handle = CreateFiber(stack_bytes, win_trampoline, f);
    if (!f->handle) {
        delete f;
        return nullptr;
    }
    return f;
}

void fiber_destroy(Fiber* f) {
    if (!f) {
        return;
    }
    if (f->handle && !f->is_thread) {
        DeleteFiber(f->handle);
    }
    delete f;
}

void fiber_switch(Fiber* /*from*/, Fiber* to) {
    // Win32 tracks the currently running fiber implicitly.
    SwitchToFiber(to->handle);
}

#elif PSY_JOBS_HAVE_FIBERS

struct Fiber {
    ucontext_t ctx{};
    FiberEntry entry = nullptr;
    void* arg = nullptr;
    void* stack = nullptr;
    bool is_thread = false;
};

static_assert(sizeof(void*) == 8, "fiber pointer-split assumes a 64-bit target");

namespace {
void posix_trampoline(unsigned hi, unsigned lo) {
    const std::uintptr_t p =
        (static_cast<std::uintptr_t>(hi) << 32) | static_cast<std::uintptr_t>(lo);
    auto* f = reinterpret_cast<Fiber*>(p);
    f->entry(f->arg);  // contract: never returns
}
}  // namespace

Fiber* fiber_for_thread() {
    auto* f = new Fiber{};
    f->is_thread = true;
#if defined(__APPLE__) && defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    getcontext(&f->ctx);
#if defined(__APPLE__) && defined(__clang__)
#  pragma clang diagnostic pop
#endif
    return f;
}

void fiber_release_thread(Fiber* thread_fiber) { delete thread_fiber; }

Fiber* fiber_create(usize stack_bytes, FiberEntry entry, void* arg) {
    auto* f = new Fiber{};
    f->entry = entry;
    f->arg = arg;

    usize sz = stack_bytes < (16u * 1024u) ? (16u * 1024u) : stack_bytes;
    sz = (sz + 63u) & ~static_cast<usize>(63u);  // round up to 64 bytes
    if (posix_memalign(&f->stack, 64, sz) != 0 || !f->stack) {
        delete f;
        return nullptr;
    }

    const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(f);
#if defined(__APPLE__) && defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wcast-function-type"
#endif
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = sz;
    f->ctx.uc_link = nullptr;
    makecontext(&f->ctx, reinterpret_cast<void (*)()>(posix_trampoline), 2,
                static_cast<unsigned>(p >> 32), static_cast<unsigned>(p & 0xffffffffu));
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif
#if defined(__APPLE__) && defined(__clang__)
#  pragma clang diagnostic pop
#endif
    return f;
}

void fiber_destroy(Fiber* f) {
    if (!f) {
        return;
    }
    std::free(f->stack);
    delete f;
}

void fiber_switch(Fiber* from, Fiber* to) {
#if defined(__APPLE__) && defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    swapcontext(&from->ctx, &to->ctx);
#if defined(__APPLE__) && defined(__clang__)
#  pragma clang diagnostic pop
#endif
}

#else  // No fiber backend available on this platform.

struct Fiber {};
Fiber* fiber_for_thread() { return nullptr; }
void fiber_release_thread(Fiber*) {}
Fiber* fiber_create(usize, FiberEntry, void*) { return nullptr; }
void fiber_destroy(Fiber*) {}
void fiber_switch(Fiber*, Fiber*) {}

#endif

}  // namespace psynder::jobs::detail
