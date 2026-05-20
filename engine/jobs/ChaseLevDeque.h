// SPDX-License-Identifier: MIT
// Psynder - lock-free Chase-Lev work-stealing deque (lane 04, internal).
//
// One deque per worker. The owning worker pushes/pops at the BOTTOM (LIFO,
// cache-hot recently-spawned work); thieves steal from the TOP (FIFO, the
// oldest, coarsest work). This is the canonical single-producer /
// multi-consumer work-stealing deque from Chase & Lev (SPAA'05), using the
// C11 memory orderings proven correct on weak memory models by Le, Pop, Cohen
// & Zappa Nardelli, "Correct and Efficient Work-Stealing for Weak Memory
// Models" (PPoPP'13).
//
// Capacity is fixed (a power of two). push() into a full ring returns false so
// the scheduler can spill the job onto its global injection queue rather than
// grow the buffer: a growable deque needs hazard-pointer / epoch reclamation
// of the retired array, complexity we deliberately avoid. 4096 slots/worker is
// far above the steady-state in-flight job count for a single frame.

#pragma once

#include "core/Types.h"

#include <atomic>

namespace psynder::jobs::detail {

template <class T, u32 CapacityPow2 = 4096>
class ChaseLevDeque {
    static_assert((CapacityPow2 & (CapacityPow2 - 1u)) == 0u, "capacity must be a power of two");
    static_assert(CapacityPow2 >= 2u, "capacity too small");
    static_assert(std::atomic<T>::is_always_lock_free, "T must be lock-free as an atomic");

public:
    ChaseLevDeque() = default;
    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

    // Owner thread only. Returns false if the ring is full (caller spills
    // the value onto another queue).
    bool push(T v) noexcept {
        const i64 b = bottom_.load(std::memory_order_relaxed);
        const i64 t = top_.load(std::memory_order_acquire);
        if (b - t >= static_cast<i64>(kCapacity)) {
            return false;  // full
        }
        slot(b).store(v, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
        return true;
    }

    // Owner thread only. Pops the most-recently pushed item. Returns false
    // when the deque is empty.
    bool pop(T& out) noexcept {
        const i64 b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        i64 t = top_.load(std::memory_order_relaxed);
        if (t <= b) {
            T v = slot(b).load(std::memory_order_relaxed);
            if (t == b) {
                // Last element: race a concurrent steal for it.
                if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                   std::memory_order_relaxed)) {
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    return false;  // a thief won the race
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
            out = v;
            return true;
        }
        // Empty: restore bottom.
        bottom_.store(b + 1, std::memory_order_relaxed);
        return false;
    }

    // Any thief thread. Steals the oldest item. Returns false on an empty
    // deque or a lost CAS race (the caller simply tries another victim).
    bool steal(T& out) noexcept {
        i64 t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const i64 b = bottom_.load(std::memory_order_acquire);
        if (t < b) {
            T v = slot(t).load(std::memory_order_relaxed);
            if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
                return false;  // lost the race
            }
            out = v;
            return true;
        }
        return false;  // empty
    }

    // Racy snapshot of the item count, for scheduling heuristics only.
    i64 size_approx() const noexcept {
        const i64 b = bottom_.load(std::memory_order_relaxed);
        const i64 t = top_.load(std::memory_order_relaxed);
        return b - t;
    }

private:
    static constexpr u32 kCapacity = CapacityPow2;
    static constexpr i64 kMask = static_cast<i64>(CapacityPow2) - 1;

    std::atomic<T>& slot(i64 i) noexcept { return ring_[static_cast<usize>(i & kMask)]; }

    // top_ and bottom_ live on separate cache lines: the owner mutates bottom_
    // every push/pop while thieves hammer top_ with CAS, and co-locating them
    // would false-share the line on every steal.
    PSY_CACHELINE_ALIGN std::atomic<i64> top_{0};
    PSY_CACHELINE_ALIGN std::atomic<i64> bottom_{0};
    PSY_CACHELINE_ALIGN std::atomic<T> ring_[CapacityPow2];
};

}  // namespace psynder::jobs::detail
