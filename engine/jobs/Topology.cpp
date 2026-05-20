// SPDX-License-Identifier: MIT
// Psynder - heterogeneous-core (P/E) topology detection. See Topology.h.

#if defined(__linux__)
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE 1  // pthread_setaffinity_np + CPU_SET macros
#  endif
#endif

#include "Topology.h"

#include "core/hardware/CpuFeatures.h"

#include <algorithm>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <pthread/qos.h>
#  include <sys/sysctl.h>
#else
#  include <cstdio>
#  include <pthread.h>
#  include <sched.h>
#  include <unistd.h>
#endif

namespace psynder::jobs::detail {

namespace {

// Host-fixed P/E split, detected once. Pool index 0 == P, 1 == E.
struct RawTopo {
    u32 perf_phys = 0;
    u32 eff_phys = 0;
    bool hetero = false;
#if defined(_WIN32)
    std::vector<GROUP_AFFINITY> pool_affinity[2];
#elif !defined(__APPLE__)
    std::vector<int> pool_cpus[2];
#endif
};

#if defined(_WIN32)

RawTopo probe_windows() {
    RawTopo r;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes == 0) {
        return r;
    }
    std::vector<u8> buffer(bytes);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &bytes)) {
        return r;
    }

    struct CoreInfo {
        GROUP_AFFINITY mask;
        BYTE eff_class;
    };
    std::vector<CoreInfo> cores;
    BYTE max_class = 0;
    DWORD offset = 0;
    while (offset < bytes) {
        auto* info =
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount >= 1) {
            const BYTE ec = info->Processor.EfficiencyClass;
            cores.push_back(CoreInfo{info->Processor.GroupMask[0], ec});
            max_class = std::max(max_class, ec);
        }
        offset += info->Size;
    }

    for (const CoreInfo& c : cores) {
        // Higher EfficiencyClass == higher performance core (per Win32 docs).
        const bool is_perf = c.eff_class == max_class;
        if (is_perf) {
            ++r.perf_phys;
            r.pool_affinity[0].push_back(c.mask);
        } else {
            ++r.eff_phys;
            r.pool_affinity[1].push_back(c.mask);
        }
    }
    r.hetero = r.eff_phys > 0 && r.perf_phys > 0;
    return r;
}

#elif defined(__APPLE__)

u32 sysctl_u32(const char* name) {
    int v = 0;
    size_t sz = sizeof(v);
    if (sysctlbyname(name, &v, &sz, nullptr, 0) == 0 && v > 0) {
        return static_cast<u32>(v);
    }
    return 0;
}

RawTopo probe_macos() {
    RawTopo r;
    const u32 nlevels = sysctl_u32("hw.nperflevels");
    if (nlevels >= 2) {
        r.perf_phys = sysctl_u32("hw.perflevel0.physicalcpu");
        r.eff_phys = sysctl_u32("hw.perflevel1.physicalcpu");
        r.hetero = r.perf_phys > 0 && r.eff_phys > 0;
    } else {
        r.perf_phys = sysctl_u32("hw.physicalcpu");
    }
    return r;
}

#else  // Linux / generic POSIX

// cpu_capacity is a unitless "bigger == more capable" value (ARM big.LITTLE,
// recent Intel-hybrid kernels). Returns 0 when the sysfs file is absent.
u32 read_cpu_capacity(int cpu) {
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
    FILE* fp = std::fopen(path, "r");
    if (!fp) {
        return 0;
    }
    unsigned long cap = 0;
    const int n = std::fscanf(fp, "%lu", &cap);
    std::fclose(fp);
    return n == 1 ? static_cast<u32>(cap) : 0;
}

RawTopo probe_linux() {
    RawTopo r;
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    const int ncpu = online > 0 ? static_cast<int>(online) : 1;

    std::vector<u32> cap(static_cast<usize>(ncpu), 0);
    u32 max_cap = 0;
    bool any_cap = false;
    for (int i = 0; i < ncpu; ++i) {
        cap[static_cast<usize>(i)] = read_cpu_capacity(i);
        if (cap[static_cast<usize>(i)] > 0) {
            any_cap = true;
            max_cap = std::max(max_cap, cap[static_cast<usize>(i)]);
        }
    }

    if (!any_cap) {
        // No capacity info -> treat as homogeneous.
        r.perf_phys = static_cast<u32>(ncpu);
        return r;
    }

    for (int i = 0; i < ncpu; ++i) {
        const bool is_perf = cap[static_cast<usize>(i)] == max_cap;
        if (is_perf) {
            ++r.perf_phys;
            r.pool_cpus[0].push_back(i);
        } else {
            ++r.eff_phys;
            r.pool_cpus[1].push_back(i);
        }
    }
    r.hetero = r.eff_phys > 0 && r.perf_phys > 0;
    return r;
}

#endif

const RawTopo& raw_topo() {
    static const RawTopo topo = [] {
#if defined(_WIN32)
        return probe_windows();
#elif defined(__APPLE__)
        return probe_macos();
#else
        return probe_linux();
#endif
    }();
    return topo;
}

}  // namespace

PoolPlan plan_pools(u32 requested_total) {
    const RawTopo& r = raw_topo();

    u32 base = requested_total;
    if (base == 0) {
        base = r.perf_phys + r.eff_phys;
        if (base == 0) {
            base = hardware::detect().cores_physical;
        }
    }
    if (base < 1) {
        base = 1;
    }

    PoolPlan p;
    // A split needs at least two workers and a detected heterogeneous host.
    if (!r.hetero || r.eff_phys == 0 || base < 2) {
        p.total_workers = base;
        p.perf_workers = base;
        p.eff_workers = 0;
        p.heterogeneous = false;
        return p;
    }

    const u32 total_phys = r.perf_phys + r.eff_phys;
    u32 perf = (base * r.perf_phys) / total_phys;
    if (perf < 1) {
        perf = 1;
    }
    if (perf >= base) {
        perf = base - 1;  // always leave at least one E worker
    }
    p.total_workers = base;
    p.perf_workers = perf;
    p.eff_workers = base - perf;
    p.heterogeneous = true;
    return p;
}

void bind_calling_thread_to_pool([[maybe_unused]] u32 pool, [[maybe_unused]] u32 slot) {
    const RawTopo& r = raw_topo();
    if (!r.hetero) {
        return;
    }
    const u32 pidx = pool > 0 ? 1u : 0u;

#if defined(_WIN32)
    const auto& aff = r.pool_affinity[pidx];
    if (aff.empty()) {
        return;
    }
    GROUP_AFFINITY ga = aff[slot % aff.size()];
    SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr);
#elif defined(__APPLE__)
    // Apple Silicon does not honour hard CPU affinity; QoS class is the
    // documented way to steer a thread onto the P (user-initiated) or E
    // (utility) cluster.
    const qos_class_t q = pidx == 0 ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY;
    pthread_set_qos_class_self_np(q, 0);
#else
    const auto& cpus = r.pool_cpus[pidx];
    if (cpus.empty()) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpus[slot % cpus.size()], &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

}  // namespace psynder::jobs::detail
