// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/heap/Heap.cpp
//
// HeapKind allocator stubs. The full per-heap allocator (sub-allocation
// inside a single device memory block, transient/aliased attachments,
// per-frame descriptor pools, command-buffer pools) lands at M1-M2 per
// DESIGN §4. For M0 (clear-color present) we don't need user-side
// buffers/textures and the swapchain images come straight from the OS
// surface, so this TU only carries the placeholder + lint-friendly
// "no mid-frame allocations" comment.
//
// Per DESIGN §4.4: NO vkAllocateMemory or [MTLDevice newBuffer*] inside
// the frame loop. All allocation goes through this file at level load.

#include "gpu/PublicGpu.h"
#include "gpu/PublicGpuInternal.h"

namespace psynder::gpu::heap {

// Heap budget per kind. Pulled into the per-level budget that the
// orchestrator decides at level load. These are documented defaults; the
// game can override per-map via the level config.
struct HeapBudget {
    HeapKind    kind;
    std::size_t bytes;
};

inline constexpr HeapBudget kDefaultBudget[] = {
    {HeapKind::DeviceLocal,  static_cast<std::size_t>(2)  * 1024u * 1024u * 1024u}, // 2  GiB
    {HeapKind::HostVisible,  static_cast<std::size_t>(256) * 1024u * 1024u},        // 256 MiB
    {HeapKind::Transient,    static_cast<std::size_t>(128) * 1024u * 1024u},        // 128 MiB
    {HeapKind::Descriptor,   static_cast<std::size_t>(16)  * 1024u * 1024u},        // 16 MiB
    {HeapKind::Command,      static_cast<std::size_t>(8)   * 1024u * 1024u},        // 8 MiB
};

// Returns the default budget for a heap kind in bytes. The backend
// allocator initializer reads this at create_device.
std::size_t default_budget_bytes(HeapKind k) noexcept {
    for (auto const& b : kDefaultBudget) {
        if (b.kind == k) return b.bytes;
    }
    return 0;
}

// Anchor symbol — keeps the TU alive in the static lib even with no
// other references at M0.
void heap_anchor() noexcept {}

} // namespace psynder::gpu::heap
