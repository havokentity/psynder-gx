// SPDX-License-Identifier: MIT
// Psynder - JobSystem scheduler. See Scheduler.h.
//
// Design summary
// --------------
// * Per-worker Chase-Lev deques (ChaseLevDeque.h) form the lock-free hot path:
//   a worker pushes/pops its own deque LIFO and steals FIFO from peers.
// * Two per-pool MPMC injection queues (mutex + std::deque) take submissions
//   from non-worker threads and cross-pool placements; they are off the steady
//   per-job path.
// * Job completion / dependency wake-up / fiber park-resume use lock-free
//   Treiber stacks with a "closed" sentinel, so a completing job that has no
//   waiters touches no lock at all.
// * Heterogeneous P/E pools: pool 0 = performance cores, pool 1 = efficiency
//   cores (Topology.h). Latency-sensitive (priority>0) work targets P;
//   throughput (priority==0) targets E when present. Workers are affinity- or
//   QoS-bound to their pool; the steal domain spans pools so neither starves.
// * Two execution backends behind an internal switch: Thread (wait-helping)
//   and Fiber (stackful, Naughty-Dog-style park/resume).
//
// Bounded-recycle assumption: the job slot pool is a ring of kJobPoolPow2
// generations. Correctness assumes fewer than that many jobs are in flight at
// once (true for any realistic frame), so a slot is never reused while a live
// handle still refers to it.

#include "Scheduler.h"

#include "ChaseLevDeque.h"
#include "Fiber.h"
#include "Topology.h"
#include "core/Types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace psynder::jobs::detail {

namespace {

constexpr u32 kJobPoolPow2 = 1u << 16;  // 65536 in-flight job slots
constexpr u32 kJobPoolMask = kJobPoolPow2 - 1u;
constexpr u32 kClosedDeps = 0xFFFFFFFFu;  // dependents-stack "job finished" marker
constexpr usize kFiberStack = 128u * 1024u;
constexpr u32 kMaxWorkers = 256u;
// Upper bound on parallel_for chunk count: keeps the fan-out (and the in-flight
// job-slot pressure + the chunk descriptor vector) far below the slot ring.
constexpr usize kMaxParChunks = usize{1} << 14;  // 16384

struct JobFiber;  // forward

struct Job {
    JobFn fn = nullptr;
    void* user = nullptr;
    const char* name = "job";
    u32 priority = 0;
    u8 pool = 0;

    std::atomic<u32> gen{0};
    // Treiber stack of dependent job ids (linked via Job::next_dependent),
    // closed to kClosedDeps when this job finishes. Doubles as the "done" flag.
    std::atomic<u32> dependents{0};
    u32 next_dependent = 0;
    // Treiber stack of fibers parked in wait(thisJob), linked via
    // JobFiber::next_waiter, closed to the sentinel when this job finishes.
    std::atomic<JobFiber*> fiber_waiters{nullptr};
    // Optional group counter (parallel_for) decremented when this job ends.
    std::atomic<i64>* group = nullptr;
};

struct Worker;  // defined below; referenced by JobFiber::owner

struct JobFiber {
    Fiber* prim = nullptr;
    u32 current_job = 0;
    JobFiber* next_waiter = nullptr;
    // Worker currently running this fiber, published by run_fiber() right
    // before switching in. Read from this stable memory slot (NOT thread_local)
    // because a fiber can be recycled onto a different worker thread, and a
    // context switch restores the fiber's saved callee-saved registers -- so a
    // cached thread_local base would be stale after migration. See run_fiber().
    Worker* owner = nullptr;
};

// Distinct address used as the "fiber waiters closed" sentinel.
JobFiber* closed_fibers() noexcept {
    static JobFiber sentinel;
    return &sentinel;
}

struct Worker {
    enum class Pending : u8 { None, Recycle, Park };

    u32 id = 0;
    u8 pool = 0;
    ChaseLevDeque<u32> deque;
    Fiber* sched_fiber = nullptr;
    Pending pending = Pending::None;
    JobFiber* pending_fiber = nullptr;
    u32 pending_wait = 0;
    std::thread thread;
};

struct Sched {
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    Backend backend = Backend::Thread;                      // backend workers were launched with
    std::atomic<Backend> active_backend{Backend::Thread};   // reflects runtime fiber->thread fallback
    PoolPlan plan;
    u32 total_workers = 0;

    std::vector<std::unique_ptr<Worker>> workers;

    std::mutex inject_mu[2];
    std::deque<u32> inject[2];

    std::mutex resumable_mu;
    std::deque<JobFiber*> resumable;

    std::mutex fiber_mu;
    std::vector<JobFiber*> free_fibers;
    std::vector<JobFiber*> all_fibers;

    std::mutex idle_mu;
    std::condition_variable idle_cv;
    std::atomic<u32> idle_count{0};

    std::unique_ptr<Job[]> jobs;
    std::atomic<u64> alloc_counter{1};
    std::atomic<i64> live_jobs{0};
};

Sched* g = nullptr;
std::mutex g_lifecycle_mu;
std::atomic<Backend> g_pref_backend{Backend::Thread};

thread_local Worker* t_worker = nullptr;
thread_local JobFiber* t_job_fiber = nullptr;

// ---- small utilities ----------------------------------------------------

u32 next_rand() noexcept {
    thread_local u32 s = 0;
    if (s == 0) {
        s = 0x9e3779b9u ^ static_cast<u32>(reinterpret_cast<std::uintptr_t>(&s));
    }
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

void wake_one_worker() {
    if (g->idle_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(g->idle_mu);
    g->idle_cv.notify_one();
}

bool pop_inject(u32 pool, u32& out) {
    if (pool >= 2u) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g->inject_mu[pool]);
    if (g->inject[pool].empty()) {
        return false;
    }
    out = g->inject[pool].front();
    g->inject[pool].pop_front();
    return true;
}

void enqueue_ready(u32 id) {
    const u8 pool = g->jobs[id - 1u].pool;
    if (t_worker && t_worker->pool == pool && t_worker->deque.push(id)) {
        wake_one_worker();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g->inject_mu[pool]);
        g->inject[pool].push_back(id);
    }
    wake_one_worker();
}

bool steal_any(Worker* self, u32& out) {
    const u32 n = g->total_workers;
    if (n == 0) {
        return false;
    }
    const u32 start = next_rand() % n;
    for (u32 k = 0; k < n; ++k) {
        Worker* w = g->workers[(start + k) % n].get();
        if (w == self) {
            continue;
        }
        u32 id = 0;
        if (w->deque.steal(id)) {
            out = id;
            return true;
        }
    }
    return false;
}

// Find one ready job for `self` (nullptr == external helper thread).
bool try_get_job(Worker* self, u32& out) {
    if (self && self->deque.pop(out)) {
        return true;
    }
    const u32 mypool = self ? self->pool : 0u;
    if (pop_inject(mypool, out)) {
        return true;
    }
    if (steal_any(self, out)) {
        return true;
    }
    const u32 other = mypool ^ 1u;
    return pop_inject(other, out);
}

bool resumable_empty() {
    std::lock_guard<std::mutex> lk(g->resumable_mu);
    return g->resumable.empty();
}

JobFiber* pop_resumable() {
    std::lock_guard<std::mutex> lk(g->resumable_mu);
    if (g->resumable.empty()) {
        return nullptr;
    }
    JobFiber* jf = g->resumable.front();
    g->resumable.pop_front();
    return jf;
}

void push_resumable(JobFiber* jf) {
    {
        std::lock_guard<std::mutex> lk(g->resumable_mu);
        g->resumable.push_back(jf);
    }
    wake_one_worker();
}

// ---- job pool + completion ----------------------------------------------

void execute_job(u32 id);  // fwd: alloc_job help-drains while the slot ring is full

JobHandle alloc_job(const JobDesc& desc) {
    // Backpressure: if the whole slot ring is live, advancing the monotonic
    // allocator would lap a still-live slot and corrupt its handle/generation.
    // Help-drain ready work (or yield) until a slot frees instead of silently
    // overwriting it. parallel_for caps its chunk fan-out far below the ring,
    // so this is a safety net for pathological direct-submit storms rather than
    // a steady-state path; draining ready jobs keeps it deadlock-free.
    while (g->live_jobs.load(std::memory_order_acquire) >= static_cast<i64>(kJobPoolPow2)) {
        u32 drain_id = 0;
        if (try_get_job(t_worker, drain_id)) {
            execute_job(drain_id);
        } else {
            std::this_thread::yield();
        }
    }

    const u64 c = g->alloc_counter.fetch_add(1, std::memory_order_relaxed);
    const u32 slot = static_cast<u32>(c) & kJobPoolMask;
    const u32 gen = static_cast<u32>(c >> 16) + 1u;

    Job& j = g->jobs[slot];
    j.fn = desc.fn;
    j.user = desc.user;
    j.name = desc.name ? desc.name : "job";
    j.priority = desc.priority;
    j.pool = static_cast<u8>(pool_for_priority(desc.priority, g->plan));
    j.next_dependent = 0;
    j.dependents.store(0, std::memory_order_relaxed);
    j.fiber_waiters.store(nullptr, std::memory_order_relaxed);
    j.group = nullptr;
    j.gen.store(gen, std::memory_order_release);

    g->live_jobs.fetch_add(1, std::memory_order_relaxed);
    return JobHandle{slot + 1u, gen};
}

bool is_complete(JobHandle h) {
    if (h.id == 0u || h.id > kJobPoolPow2) {
        return true;
    }
    const Job& j = g->jobs[h.id - 1u];
    if (j.gen.load(std::memory_order_acquire) != h.gen) {
        return true;  // slot recycled => the awaited job long finished
    }
    return j.dependents.load(std::memory_order_acquire) == kClosedDeps;
}

// Register `dependent` to run when `dep` finishes. Returns false if `dep` is
// already finished (caller should enqueue `dependent` immediately).
bool push_dependent(u32 dep_id, u32 dependent_id) {
    std::atomic<u32>& head = g->jobs[dep_id - 1u].dependents;
    u32 cur = head.load(std::memory_order_acquire);
    for (;;) {
        if (cur == kClosedDeps) {
            return false;
        }
        g->jobs[dependent_id - 1u].next_dependent = cur;
        if (head.compare_exchange_weak(cur, dependent_id, std::memory_order_release,
                                       std::memory_order_acquire)) {
            return true;
        }
    }
}

// Park `jf` on job `id`. Returns false if the job already finished.
bool push_fiber_waiter(u32 id, JobFiber* jf) {
    std::atomic<JobFiber*>& head = g->jobs[id - 1u].fiber_waiters;
    JobFiber* cur = head.load(std::memory_order_acquire);
    for (;;) {
        if (cur == closed_fibers()) {
            return false;
        }
        jf->next_waiter = cur;
        if (head.compare_exchange_weak(cur, jf, std::memory_order_release,
                                       std::memory_order_acquire)) {
            return true;
        }
    }
}

// PSY_NOINLINE: finish_job reads thread_local state (via enqueue_ready's
// local-deque fast path). Keeping it out of line guarantees the TLS base is
// re-derived on entry -- on the actual current thread -- rather than reusing a
// caller's register-cached base that may be stale after a fiber migration.
PSY_NOINLINE void finish_job(u32 id) {
    Job& j = g->jobs[id - 1u];

    // Close + drain dependents: their dependency is now satisfied.
    u32 dhead = j.dependents.exchange(kClosedDeps, std::memory_order_acq_rel);
    while (dhead != 0u && dhead != kClosedDeps) {
        const u32 next = g->jobs[dhead - 1u].next_dependent;
        enqueue_ready(dhead);
        dhead = next;
    }

    // Close + drain parked fibers: make them resumable.
    JobFiber* fhead = j.fiber_waiters.exchange(closed_fibers(), std::memory_order_acq_rel);
    while (fhead != nullptr && fhead != closed_fibers()) {
        JobFiber* next = fhead->next_waiter;
        push_resumable(fhead);
        fhead = next;
    }

    if (j.group) {
        j.group->fetch_sub(1, std::memory_order_acq_rel);
    }
    g->live_jobs.fetch_sub(1, std::memory_order_acq_rel);
    wake_one_worker();
}

void execute_job(u32 id) {
    Job& j = g->jobs[id - 1u];
    const JobFn fn = j.fn;
    void* const user = j.user;
    if (fn) {
        fn(user);
    }
    finish_job(id);
}

// ---- idle / help loops ---------------------------------------------------

void idle_wait(Worker* /*self*/) {
    g->idle_count.fetch_add(1, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(g->idle_mu);
        // No predicate: a plain timed wait returns on notify_one() from
        // wake_one_worker() (so submitting work actually cuts latency), on a
        // spurious wakeup, or at the timeout. The worker loop re-checks for
        // ready work and the stopping flag on return, so any wakeup is safe;
        // the short timeout is just a backstop against a missed notification.
        g->idle_cv.wait_for(lk, std::chrono::microseconds(300));
    }
    g->idle_count.fetch_sub(1, std::memory_order_release);
}

template <class Pred>
void help_until(Pred done) {
    Worker* self = t_worker;
    u32 spins = 0;
    while (!done()) {
        u32 id = 0;
        if (try_get_job(self, id)) {
            execute_job(id);
            spins = 0;
        } else if (++spins < 64u) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

// ---- fiber backend -------------------------------------------------------

JobFiber* acquire_free_fiber();
void release_free_fiber(JobFiber* jf);

void fiber_entry(void* arg) {
    auto* self = static_cast<JobFiber*>(arg);
    for (;;) {
        execute_job(self->current_job);
        // self->owner (memory) is reloaded after the opaque context switch and
        // is correct even if this fiber migrated to a different worker thread;
        // t_worker would be read through a stale, register-cached TLS base.
        Worker* w = self->owner;
        w->pending = Worker::Pending::Recycle;
        w->pending_fiber = self;
        fiber_switch(self->prim, w->sched_fiber);
        // Resumed here when re-acquired with a fresh current_job.
    }
}

JobFiber* acquire_free_fiber() {
    {
        std::lock_guard<std::mutex> lk(g->fiber_mu);
        if (!g->free_fibers.empty()) {
            JobFiber* jf = g->free_fibers.back();
            g->free_fibers.pop_back();
            return jf;
        }
    }
    auto* jf = new JobFiber{};
    jf->prim = fiber_create(kFiberStack, &fiber_entry, jf);
    if (!jf->prim) {
        delete jf;
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g->fiber_mu);
        g->all_fibers.push_back(jf);
    }
    return jf;
}

void release_free_fiber(JobFiber* jf) {
    std::lock_guard<std::mutex> lk(g->fiber_mu);
    g->free_fibers.push_back(jf);
}

void handle_pending(Worker* self) {
    if (self->pending == Worker::Pending::Recycle) {
        JobFiber* jf = self->pending_fiber;
        self->pending = Worker::Pending::None;
        self->pending_fiber = nullptr;
        release_free_fiber(jf);
    } else if (self->pending == Worker::Pending::Park) {
        JobFiber* jf = self->pending_fiber;
        const u32 wait_id = self->pending_wait;
        self->pending = Worker::Pending::None;
        self->pending_fiber = nullptr;
        self->pending_wait = 0;
        // jf has fully switched out now; safe to publish it as a waiter.
        if (!push_fiber_waiter(wait_id, jf)) {
            push_resumable(jf);  // dependency already satisfied
        }
    }
}

void run_fiber(Worker* self, JobFiber* jf) {
    jf->owner = self;  // published so the fiber can find its current worker post-migration
    t_job_fiber = jf;
    fiber_switch(self->sched_fiber, jf->prim);
    t_job_fiber = nullptr;
}

void fiber_sched_loop(Worker* self) {
    for (;;) {
        handle_pending(self);
        if (JobFiber* jf = pop_resumable()) {
            run_fiber(self, jf);
            continue;
        }
        u32 id = 0;
        if (try_get_job(self, id)) {
            JobFiber* jf = acquire_free_fiber();
            if (!jf) {
                execute_job(id);  // out-of-fibers fallback
                continue;
            }
            jf->current_job = id;
            run_fiber(self, jf);
            continue;
        }
        if (g->stopping.load(std::memory_order_acquire) &&
            g->live_jobs.load(std::memory_order_acquire) == 0 && resumable_empty()) {
            handle_pending(self);
            break;
        }
        idle_wait(self);
    }
}

void thread_sched_loop(Worker* self) {
    for (;;) {
        u32 id = 0;
        if (try_get_job(self, id)) {
            execute_job(id);
            continue;
        }
        if (g->stopping.load(std::memory_order_acquire) &&
            g->live_jobs.load(std::memory_order_acquire) == 0) {
            break;
        }
        idle_wait(self);
    }
}

void worker_thread_main(Worker* self) {
    t_worker = self;
    const u32 slot_in_pool = self->pool == 0 ? self->id : self->id - g->plan.perf_workers;
    bind_calling_thread_to_pool(self->pool, slot_in_pool);

    if (g->backend == Backend::Fiber) {
        self->sched_fiber = fiber_for_thread();
        if (self->sched_fiber) {
            fiber_sched_loop(self);
            fiber_release_thread(self->sched_fiber);
            self->sched_fiber = nullptr;
            t_worker = nullptr;
            return;
        }
        // fiber_for_thread() failed -> degrade to the thread backend, and
        // publish the fallback so sched_active_backend() reports it honestly.
        g->active_backend.store(Backend::Thread, std::memory_order_release);
    }
    thread_sched_loop(self);
    t_worker = nullptr;
}

// ---- parallel_for chunks -------------------------------------------------

struct ParChunk {
    const std::function<void(usize, usize)>* body;
    usize lo;
    usize hi;
};

void par_chunk_trampoline(void* p) noexcept {
    auto* c = static_cast<ParChunk*>(p);
    (*c->body)(c->lo, c->hi);
}

u32 submit_grouped(const JobDesc& desc, std::atomic<i64>* group) {
    const JobHandle h = alloc_job(desc);
    g->jobs[h.id - 1u].group = group;
    enqueue_ready(h.id);
    return h.id;
}

}  // namespace

// ---- exported API --------------------------------------------------------

bool fibers_supported() noexcept { return PSY_JOBS_HAVE_FIBERS != 0; }

void set_preferred_backend(Backend b) noexcept {
    g_pref_backend.store(b, std::memory_order_relaxed);
}

Backend preferred_backend() noexcept { return g_pref_backend.load(std::memory_order_relaxed); }

bool sched_running() noexcept { return g != nullptr && g->running.load(std::memory_order_acquire); }

Backend sched_active_backend() noexcept {
    return g ? g->active_backend.load(std::memory_order_acquire) : Backend::Thread;
}

u32 sched_worker_count() noexcept { return g ? g->total_workers : 0u; }

u32 sched_current_worker() noexcept { return t_worker ? t_worker->id : ~0u; }

void sched_start(u32 worker_count) {
    std::lock_guard<std::mutex> lk(g_lifecycle_mu);
    if (g && g->running.load(std::memory_order_acquire)) {
        return;  // idempotent
    }

    auto* s = new Sched{};
    s->plan = plan_pools(worker_count);
    s->total_workers = s->plan.total_workers;
    if (s->total_workers < 1u) {
        s->total_workers = 1u;
    }
    if (s->total_workers > kMaxWorkers) {
        s->total_workers = kMaxWorkers;
    }

    Backend backend = preferred_backend();
    if (backend == Backend::Fiber && !fibers_supported()) {
        backend = Backend::Thread;
    }
    if (const char* env = std::getenv("PSY_JOBS_BACKEND")) {
        if (std::strcmp(env, "thread") == 0) {
            backend = Backend::Thread;
        } else if (std::strcmp(env, "fiber") == 0 && fibers_supported()) {
            backend = Backend::Fiber;
        }
    }
    s->backend = backend;
    s->active_backend.store(backend, std::memory_order_relaxed);

    s->jobs = std::make_unique<Job[]>(kJobPoolPow2);
    s->stopping.store(false, std::memory_order_relaxed);
    s->running.store(true, std::memory_order_release);

    g = s;

    // Build all workers before launching any thread so cross-worker stealing
    // never dereferences a half-built table.
    for (u32 i = 0; i < s->total_workers; ++i) {
        auto w = std::make_unique<Worker>();
        w->id = i;
        w->pool = (s->plan.heterogeneous && i >= s->plan.perf_workers) ? u8{1} : u8{0};
        s->workers.push_back(std::move(w));
    }
    for (u32 i = 0; i < s->total_workers; ++i) {
        Worker* self = s->workers[i].get();
        self->thread = std::thread(worker_thread_main, self);
    }
}

void sched_stop() {
    std::lock_guard<std::mutex> lk(g_lifecycle_mu);
    if (!g || !g->running.load(std::memory_order_acquire)) {
        return;
    }
    Sched* s = g;

    s->stopping.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> ilk(s->idle_mu);
        s->idle_cv.notify_all();
    }
    for (auto& w : s->workers) {
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }

    for (JobFiber* jf : s->all_fibers) {
        fiber_destroy(jf->prim);
        delete jf;
    }

    s->running.store(false, std::memory_order_release);
    g = nullptr;
    delete s;
}

JobHandle sched_submit(const JobDesc& desc, JobHandle dep) {
    const JobHandle h = alloc_job(desc);
    if (dep.valid() && !is_complete(dep)) {
        if (push_dependent(dep.id, h.id)) {
            return h;  // will be enqueued when dep finishes
        }
        // dep finished between the check and here -> fall through to enqueue.
    }
    enqueue_ready(h.id);
    return h;
}

void sched_wait(JobHandle h) {
    if (h.id == 0u || is_complete(h)) {
        return;
    }
    if (g->backend == Backend::Fiber && t_job_fiber != nullptr) {
        Worker* w = t_worker;
        JobFiber* jf = t_job_fiber;
        w->pending = Worker::Pending::Park;
        w->pending_fiber = jf;
        w->pending_wait = h.id;
        fiber_switch(jf->prim, w->sched_fiber);
        return;  // resumed once h finished
    }
    help_until([&] { return is_complete(h); });
}

void sched_parallel_for(usize begin, usize end, usize grain,
                        const std::function<void(usize, usize)>& body) {
    if (begin >= end || !body) {
        return;
    }
    usize eff_grain = grain ? grain : usize{1};
    const usize span = end - begin;
    usize nchunks = (span + eff_grain - 1) / eff_grain;
    // Coarsen the grain if the requested chunk count would overrun the cap. The
    // body still covers [begin,end) exactly once, so this is over-decomposition
    // tuning, not a behaviour change.
    if (nchunks > kMaxParChunks) {
        eff_grain = (span + kMaxParChunks - 1) / kMaxParChunks;
        nchunks = (span + eff_grain - 1) / eff_grain;
    }
    if (nchunks <= 1) {
        body(begin, end);
        return;
    }

    std::atomic<i64> remaining{static_cast<i64>(nchunks)};
    std::vector<ParChunk> chunks(nchunks);
    for (usize k = 0; k < nchunks; ++k) {
        const usize lo = begin + k * eff_grain;
        usize hi = lo + eff_grain;
        if (hi > end) {
            hi = end;
        }
        chunks[k] = ParChunk{&body, lo, hi};

        JobDesc d{};
        d.fn = &par_chunk_trampoline;
        d.user = &chunks[k];
        d.name = "parallel_for.chunk";
        d.priority = 0;
        submit_grouped(d, &remaining);
    }

    help_until([&] { return remaining.load(std::memory_order_acquire) <= 0; });
}

}  // namespace psynder::jobs::detail
