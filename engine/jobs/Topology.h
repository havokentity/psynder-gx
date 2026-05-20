// SPDX-License-Identifier: MIT
// Psynder - heterogeneous-core (P/E) topology detection (lane 04, internal).
//
// The shared CPU probe in engine/core/hardware (psynder::hardware::detect())
// reports core/cache counts and ISA flags but does NOT classify performance
// (P) vs efficiency (E) cores. This jobs-local layer adds that classification
// on top of the core probe so the scheduler can run latency-sensitive work on
// P cores and throughput/background work on E cores. It is deliberately scoped
// to the jobs lane; a future refactor may promote the P/E fields into
// core::CpuFeatures (tracked in the lane-04 PR notes).
//
// Detection paths:
//   - Windows: GetLogicalProcessorInformationEx(RelationProcessorCore) exposes
//     PROCESSOR_RELATIONSHIP::EfficiencyClass per physical core (nonzero only
//     on hybrid silicon; higher value == higher performance).
//   - macOS:   sysctl hw.nperflevels + hw.perflevelN.physicalcpu. Level 0 is
//     the performance tier, level 1 the efficiency tier.
//   - Linux:   /sys/devices/system/cpu/cpuN/cpu_capacity (ARM big.LITTLE and
//     recent Intel-hybrid kernels). The max-capacity tier is P, lower is E.
// Any host that does not report a split is treated as homogeneous (one pool).

#pragma once

#include "core/Types.h"

namespace psynder::jobs::detail {

// Worker partition derived from CPU topology. Pool 0 == performance (P) cores,
// pool 1 == efficiency (E) cores. On homogeneous CPUs there is a single pool
// (eff_workers == 0, heterogeneous == false).
struct PoolPlan {
    u32 total_workers = 1;
    u32 perf_workers = 1;   // pool 0 (P)
    u32 eff_workers = 0;    // pool 1 (E)
    bool heterogeneous = false;
};

// Build a worker plan. `requested_total == 0` autodetects one worker per
// physical core, split across P/E by the detected ratio. A nonzero request is
// honoured and split proportionally. The result always has total_workers >= 1
// and perf_workers >= 1.
PoolPlan plan_pools(u32 requested_total);

// Pure scheduling policy: which pool a job of the given priority targets.
// Latency-sensitive work (priority > 0) -> P (pool 0). Throughput / background
// work (priority == 0) -> E (pool 1) when efficiency cores exist, else P.
// Pure and side-effect free so it is unit-testable on any host.
constexpr u32 pool_for_priority(u32 priority, const PoolPlan& plan) noexcept {
    if (!plan.heterogeneous || plan.eff_workers == 0) {
        return 0;
    }
    return priority > 0 ? 0u : 1u;
}

// Best-effort: pin / QoS-hint the calling worker thread onto the cores of
// `pool`. `slot` is the worker's index within its pool, used to spread workers
// across that pool's cores. Failures are swallowed - affinity is a scheduling
// optimisation, never a correctness requirement. Must be called after
// plan_pools() (which caches the detected core masks).
void bind_calling_thread_to_pool(u32 pool, u32 slot);

}  // namespace psynder::jobs::detail
