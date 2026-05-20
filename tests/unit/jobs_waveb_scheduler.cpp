// SPDX-License-Identifier: MIT
// Unit tests for the lane-04 Wave B JobSystem backend: Chase-Lev work-stealing
// scheduler, stackful-fiber execution backend, and heterogeneous P/E pools.
//
// All TEST_CASE names are ASCII-only (see AGENTS.md): ctest replays each
// discovered name back as a command-line filter and a non-ASCII name is
// mangled by the Windows CRT argv decoder.
//
// Determinism note: the parallel reduction below accumulates in u64. Integer
// addition is associative, so the parallel result is bit-for-bit identical to
// the single-threaded baseline regardless of how the scheduler distributes
// chunks across workers - that is the property we assert.

#include "jobs/JobSystem.h"
#include "jobs/Scheduler.h"
#include "jobs/Topology.h"

#include <atomic>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace jobs = psynder::jobs;
namespace detail = psynder::jobs::detail;
using psynder::u32;
using psynder::u64;
using psynder::usize;

namespace {

// RAII: start the pool with a chosen backend, always stop it on scope exit
// (Catch2 REQUIRE throws on failure, so a plain stop() call could be skipped).
struct PoolGuard {
    PoolGuard(detail::Backend backend, u32 workers) {
        detail::set_preferred_backend(backend);
        jobs::JobSystem::Get().start(workers);
    }
    ~PoolGuard() { jobs::JobSystem::Get().stop(); }
    PoolGuard(const PoolGuard&) = delete;
    PoolGuard& operator=(const PoolGuard&) = delete;
};

// Run `body(backend)` once per execution backend available on this host.
template <class Body>
void for_each_backend(Body&& body) {
    body(detail::Backend::Thread);
    if (detail::fibers_supported()) {
        body(detail::Backend::Fiber);
    }
}

}  // namespace

// ---- pure policy (no pool needed) ---------------------------------------

TEST_CASE("jobs: P/E pool policy routes by priority", "[jobs][pe]") {
    const detail::PoolPlan homo{4, 4, 0, false};
    REQUIRE(detail::pool_for_priority(0, homo) == 0u);
    REQUIRE(detail::pool_for_priority(7, homo) == 0u);  // single pool: always P

    const detail::PoolPlan het{8, 6, 2, true};
    REQUIRE(detail::pool_for_priority(0, het) == 1u);    // throughput -> E
    REQUIRE(detail::pool_for_priority(1, het) == 0u);    // latency -> P
    REQUIRE(detail::pool_for_priority(255, het) == 0u);  // latency -> P
}

TEST_CASE("jobs: pool plan is internally consistent", "[jobs][pe]") {
    const detail::PoolPlan autop = detail::plan_pools(0);
    REQUIRE(autop.total_workers >= 1u);
    REQUIRE(autop.perf_workers >= 1u);
    REQUIRE(autop.perf_workers + autop.eff_workers == autop.total_workers);

    const detail::PoolPlan p4 = detail::plan_pools(4);
    REQUIRE(p4.total_workers == 4u);
    REQUIRE(p4.perf_workers >= 1u);
    REQUIRE(p4.perf_workers + p4.eff_workers == 4u);
    if (p4.heterogeneous) {
        REQUIRE(p4.eff_workers >= 1u);  // a real split keeps at least one E worker
    }
}

// ---- correctness under both backends ------------------------------------

TEST_CASE("jobs: parallel reduction matches single-thread bit-for-bit", "[jobs][determinism]") {
    constexpr usize kN = 1u << 20;  // 1,048,576 elements
    std::vector<u64> data(kN);
    for (usize i = 0; i < kN; ++i) {
        data[i] = static_cast<u64>(i) * 2654435761ull + 1ull;
    }
    u64 baseline = 0;
    for (usize i = 0; i < kN; ++i) {
        baseline += data[i];
    }

    for_each_backend([&](detail::Backend backend) {
        PoolGuard guard(backend, 8);
        REQUIRE(jobs::JobSystem::Get().worker_count() >= 1u);

        constexpr usize kParts = 64;
        std::vector<u64> partials(kParts, 0);
        jobs::JobSystem::Get().parallel_for(0, kParts, 1, [&](usize a, usize b) {
            for (usize p = a; p < b; ++p) {
                const usize lo = p * kN / kParts;
                const usize hi = (p + 1) * kN / kParts;
                u64 s = 0;
                for (usize i = lo; i < hi; ++i) {
                    s += data[i];
                }
                partials[p] = s;
            }
        });

        u64 parallel = 0;
        for (usize p = 0; p < kParts; ++p) {
            parallel += partials[p];
        }
        REQUIRE(parallel == baseline);
    });
}

TEST_CASE("jobs: parallel_for visits every index exactly once", "[jobs]") {
    constexpr usize kN = 100000;
    for_each_backend([&](detail::Backend backend) {
        PoolGuard guard(backend, 4);
        std::vector<unsigned char> visited(kN, 0);
        jobs::JobSystem::Get().parallel_for(0, kN, 1000, [&](usize a, usize b) {
            for (usize i = a; i < b; ++i) {
                visited[i] = static_cast<unsigned char>(visited[i] + 1);
            }
        });
        usize ones = 0;
        for (usize i = 0; i < kN; ++i) {
            if (visited[i] == 1) {
                ++ones;
            }
        }
        REQUIRE(ones == kN);
    });
}

TEST_CASE("jobs: submit then wait runs the job once", "[jobs]") {
    for_each_backend([&](detail::Backend backend) {
        PoolGuard guard(backend, 4);
        std::atomic<int> ran{0};
        jobs::JobDesc d{};
        d.fn = [](void* u) noexcept { static_cast<std::atomic<int>*>(u)->fetch_add(1); };
        d.user = &ran;
        d.name = "unit.submit";
        const jobs::JobHandle h = jobs::JobSystem::Get().submit(d);
        jobs::JobSystem::Get().wait(h);
        REQUIRE(ran.load() == 1);
    });
}

TEST_CASE("jobs: dependency chain preserves submission order", "[jobs]") {
    struct Chain {
        std::atomic<int> tick{0};
        std::vector<int> order;
    };
    for_each_backend([&](detail::Backend backend) {
        PoolGuard guard(backend, 4);
        constexpr int kLen = 32;
        Chain chain;
        chain.order.assign(kLen, -1);

        struct Step {
            Chain* chain;
            int index;
        };
        std::vector<Step> steps(static_cast<usize>(kLen));

        auto step_fn = [](void* u) noexcept {
            auto* s = static_cast<Step*>(u);
            s->chain->order[static_cast<usize>(s->index)] = s->chain->tick.fetch_add(1);
        };

        jobs::JobHandle prev{};
        for (int i = 0; i < kLen; ++i) {
            steps[static_cast<usize>(i)] = Step{&chain, i};
            jobs::JobDesc d{};
            d.fn = step_fn;
            d.user = &steps[static_cast<usize>(i)];
            d.name = "unit.chain";
            prev = jobs::JobSystem::Get().submit(d, prev);
        }
        jobs::JobSystem::Get().wait(prev);

        for (int i = 0; i < kLen; ++i) {
            REQUIRE(chain.order[static_cast<usize>(i)] == i);
        }
    });
}

TEST_CASE("jobs: nested waits do not block worker threads", "[jobs][fiber]") {
    // More parents simultaneously waiting than there are workers. If wait()
    // blocked the worker OS thread this would deadlock once every worker is
    // parked with no thread left to run the children. Both the wait-helping
    // (Thread) and fiber backends must keep making progress.
    struct Tree {
        std::atomic<int> children{0};
        std::atomic<int> parents{0};
    };
    for_each_backend([&](detail::Backend backend) {
        PoolGuard guard(backend, 2);  // deliberately fewer workers than parents

        Tree tree;
        constexpr int kParents = 16;

        auto parent_fn = [](void* u) noexcept {
            auto* t = static_cast<Tree*>(u);
            jobs::JobDesc c{};
            c.fn = [](void* cu) noexcept { static_cast<Tree*>(cu)->children.fetch_add(1); };
            c.user = u;
            c.name = "unit.child";
            const jobs::JobHandle hc = jobs::JobSystem::Get().submit(c);
            jobs::JobSystem::Get().wait(hc);  // nested wait inside a running job
            t->parents.fetch_add(1);
        };

        std::vector<jobs::JobHandle> parents(static_cast<usize>(kParents));
        for (int i = 0; i < kParents; ++i) {
            jobs::JobDesc d{};
            d.fn = parent_fn;
            d.user = &tree;
            d.name = "unit.parent";
            parents[static_cast<usize>(i)] = jobs::JobSystem::Get().submit(d);
        }
        for (auto h : parents) {
            jobs::JobSystem::Get().wait(h);
        }

        REQUIRE(tree.parents.load() == kParents);
        REQUIRE(tree.children.load() == kParents);
    });
}

TEST_CASE("jobs: fiber backend actually engages when selected", "[jobs][fiber]") {
    if (!detail::fibers_supported()) {
        SUCCEED("fibers unavailable on this platform");
        return;
    }
    PoolGuard guard(detail::Backend::Fiber, 4);
    REQUIRE(detail::sched_active_backend() == detail::Backend::Fiber);

    std::atomic<int> ran{0};
    jobs::JobDesc d{};
    d.fn = [](void* u) noexcept { static_cast<std::atomic<int>*>(u)->fetch_add(1); };
    d.user = &ran;
    const jobs::JobHandle h = jobs::JobSystem::Get().submit(d);
    jobs::JobSystem::Get().wait(h);
    REQUIRE(ran.load() == 1);
}

TEST_CASE("jobs: priority-tagged jobs all complete", "[jobs][pe]") {
    for_each_backend([&](detail::Backend backend) {
        PoolGuard guard(backend, 4);
        std::atomic<int> done{0};
        constexpr int kEach = 200;
        std::vector<jobs::JobHandle> handles;
        handles.reserve(static_cast<usize>(kEach) * 2u);

        auto bump = [](void* u) noexcept { static_cast<std::atomic<int>*>(u)->fetch_add(1); };
        for (int i = 0; i < kEach; ++i) {
            jobs::JobDesc lat{};  // latency-sensitive -> P pool
            lat.fn = bump;
            lat.user = &done;
            lat.priority = 8;
            handles.push_back(jobs::JobSystem::Get().submit(lat));

            jobs::JobDesc thr{};  // throughput/background -> E pool (if present)
            thr.fn = bump;
            thr.user = &done;
            thr.priority = 0;
            handles.push_back(jobs::JobSystem::Get().submit(thr));
        }
        for (auto h : handles) {
            jobs::JobSystem::Get().wait(h);
        }
        REQUIRE(done.load() == kEach * 2);
    });
}

TEST_CASE("jobs: high-throughput churn across pool restarts", "[jobs]") {
    // Drives many fiber acquire/recycle cycles and pool restarts in a single
    // run. Fibers get recycled onto different worker threads, so this exercises
    // cross-thread fiber migration (a single non-repeated pass previously hid
    // the migration hazard). Both backends must stay correct under churn.
    for_each_backend([&](detail::Backend backend) {
        constexpr int kRounds = 24;
        constexpr int kJobs = 256;
        for (int round = 0; round < kRounds; ++round) {
            PoolGuard guard(backend, 4);
            std::atomic<int> done{0};
            std::vector<jobs::JobHandle> handles;
            handles.reserve(static_cast<usize>(kJobs));
            auto bump = [](void* u) noexcept {
                static_cast<std::atomic<int>*>(u)->fetch_add(1);
            };
            for (int i = 0; i < kJobs; ++i) {
                jobs::JobDesc d{};
                d.fn = bump;
                d.user = &done;
                d.name = "unit.churn";
                d.priority = static_cast<psynder::u32>(i & 1);  // spread across P/E pools
                handles.push_back(jobs::JobSystem::Get().submit(d));
            }
            for (auto h : handles) {
                jobs::JobSystem::Get().wait(h);
            }
            REQUIRE(done.load() == kJobs);
        }
    });
}

// ---- drop-in compatibility (pool never started) -------------------------

TEST_CASE("jobs: synchronous fallback runs work when pool is not started", "[jobs]") {
    // No JobSystem::start() here: must behave like the Phase-0 synchronous
    // scaffold so engine code/tests that never start the pool keep working.
    REQUIRE(jobs::JobSystem::Get().worker_count() == 1u);

    std::atomic<int> ran{0};
    jobs::JobDesc d{};
    d.fn = [](void* u) noexcept { static_cast<std::atomic<int>*>(u)->fetch_add(1); };
    d.user = &ran;
    const jobs::JobHandle h = jobs::JobSystem::Get().submit(d);
    jobs::JobSystem::Get().wait(h);
    REQUIRE(ran.load() == 1);  // ran inline at submit()

    constexpr usize kN = 4096;
    std::vector<int> v(kN, 0);
    jobs::JobSystem::Get().parallel_for(0, kN, 256, [&](usize a, usize b) {
        for (usize i = a; i < b; ++i) {
            v[i] = 1;
        }
    });
    usize sum = 0;
    for (usize i = 0; i < kN; ++i) {
        sum += static_cast<usize>(v[i]);
    }
    REQUIRE(sum == kN);
}
