// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/PublicGpuInternal.h
//
// INTERNAL header for lane 07. Defines:
//   * RefCountedBase — intrusive refcount + frames-in-flight retire-frame tag
//   * Concrete struct layout for Device / Heap / Buffer / Texture / Sampler
//     / Pipeline / DescriptorSet / CmdBuffer / AccelerationStructure
//   * Backend dispatch table (Vulkan vs Metal selected at compile time)
//
// Other lanes MUST NOT include this header. They only see PublicGpu.h.
// The struct types in PublicGpu.h are forward-declared opaque pointers.
// This header gives them concrete bodies inside the lane.

#pragma once

#include "gpu/PublicGpu.h"

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace psynder::gpu {

// ─── Forward decls used inside the lane ─────────────────────────────────
class Backend;          // abstract — implemented by VkBackend / MtlBackend

// ─── Intrusive refcount with frames-in-flight retire tag ────────────────
//
// All GPU resources inherit from this. Handle<T> manipulates the refcount.
// `retire_frame` records the most recent frame index when this resource
// was last referenced by a submitted command buffer; the device's
// per-frame reaper checks completed-frame index against this before
// reclaiming the underlying API resource.
class RefCountedBase {
public:
    RefCountedBase() noexcept = default;
    RefCountedBase(const RefCountedBase&)            = delete;
    RefCountedBase& operator=(const RefCountedBase&) = delete;

    void add_ref() const noexcept {
        refs_.fetch_add(1, std::memory_order_relaxed);
    }

    // Returns true iff this call dropped the refcount to zero. The caller
    // (Handle<T>::~Handle) is responsible for arranging deferred destroy
    // via the owning device once the GPU has retired the resource.
    bool release_ref() const noexcept {
        return refs_.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }

    std::uint32_t refcount() const noexcept {
        return refs_.load(std::memory_order_relaxed);
    }

    // Frames-in-flight tracking. The CmdBuffer recorder bumps this to the
    // device's current_frame_index whenever it binds the resource. The
    // retire pump only deletes resources whose retire_frame <=
    // last_completed_frame.
    void mark_used_frame(std::uint64_t frame) noexcept {
        std::uint64_t cur = retire_frame_.load(std::memory_order_relaxed);
        while (cur < frame
               && !retire_frame_.compare_exchange_weak(
                      cur, frame,
                      std::memory_order_release,
                      std::memory_order_relaxed)) {
            // retry
        }
    }
    std::uint64_t retire_frame() const noexcept {
        return retire_frame_.load(std::memory_order_acquire);
    }

    // The owning device pointer; used by Handle<T>::~Handle to queue the
    // deferred destroy. Set by the backend factory on creation.
    Device* owner() const noexcept { return owner_; }
    void    set_owner(Device* d) noexcept { owner_ = d; }

    // Virtual dtor — backends derive concrete resources from these. Made
    // public so the backend's destroy_resource() can `delete` through a
    // RefCountedBase*; Handle<T> never deletes directly.
    virtual ~RefCountedBase() = default;

private:
    mutable std::atomic<std::uint32_t> refs_ {0};
    std::atomic<std::uint64_t>         retire_frame_ {0};
    Device*                            owner_ {nullptr};

    // Need to live above the `protected:` line in some compilers
    // (clang's class layout is fine, but keep the trailing private
    // close-brace explicit here for clarity).
};

// ─── Public opaque types' concrete bodies ───────────────────────────────
//
// These are POD-ish bases. The Vulkan / Metal backends derive concrete
// classes from them and store API-specific fields. The base class only
// holds API-neutral state visible to render lanes through getters that
// live in this internal header.

struct Heap     : RefCountedBase { HeapKind kind {HeapKind::DeviceLocal}; std::size_t capacity_bytes {0}; };
struct Buffer   : RefCountedBase { BufferDesc desc {}; };
struct Texture  : RefCountedBase { TextureDesc desc {}; };
struct Sampler  : RefCountedBase {};
struct Pipeline : RefCountedBase {};
struct DescriptorSet : RefCountedBase {};
struct AccelerationStructure : RefCountedBase {};

// PublicGpu.h forward-declares these two; give them empty bodies in the
// internal header so M5 (RT) work has somewhere to grow them. Until then
// the by-const-ref ABI on the public surface is preserved.
struct BlasDesc {};
struct TlasDesc {};

struct CmdBuffer : RefCountedBase {
    // The recorder; render lanes don't see this in PublicGpu.h. The
    // backend's cmd_open returns a derived type pointer through this
    // base. cmd_submit downcasts internally.
    bool open    {false};
    bool submitted {false};
};

// ─── Device — opaque to other lanes, defined here for the backend ───────
//
// Device is NOT refcounted — it's owned by the application and torn down
// at shutdown. It holds the backend pointer + frame-in-flight bookkeeping.
struct Device {
    DeviceDesc    desc {};
    Backend*      backend {nullptr};
    std::uint64_t current_frame_index    {0};
    std::uint64_t last_completed_frame   {0};
    std::uint32_t swapchain_width        {0};
    std::uint32_t swapchain_height       {0};
    bool          supports_rt            {false};
    bool          supports_mesh          {false};
    bool          unified_memory         {false};
    const char*   device_name_cstr       {""};
};

// ─── Backend dispatch table (single virtual indirection per frame call) ─
class Backend {
public:
    virtual ~Backend() = default;

    virtual bool init(Device* dev) = 0;
    virtual void shutdown(Device* dev) = 0;

    virtual bool begin_frame(Device* dev) = 0;
    virtual void end_frame(Device* dev) = 0;

    virtual CmdBuffer* cmd_open(Device* dev) = 0;
    virtual void       cmd_submit(Device* dev, CmdBuffer* cmd) = 0;

    virtual void resize_swapchain(Device* dev, std::uint32_t w, std::uint32_t h) = 0;

    virtual Buffer*  create_buffer (Device* dev, const BufferDesc&)  = 0;
    virtual Texture* create_texture(Device* dev, const TextureDesc&) = 0;
    virtual Sampler* create_sampler(Device* dev)                     = 0;

    virtual void*    buffer_map  (Buffer*) = 0;
    virtual void     buffer_unmap(Buffer*) = 0;

    // RT — M5 work. Backend may return nullptr for an "unsupported" stub.
    virtual AccelerationStructure* create_blas(Device*, const BlasDesc&) { return nullptr; }
    virtual AccelerationStructure* create_tlas(Device*, const TlasDesc&) { return nullptr; }
    virtual void                   refit_tlas (Device*, AccelerationStructure*) {}

    // Deferred destroy entry — called by Handle<T>::~Handle once refcount
    // hits zero and the GPU has retired the resource.
    virtual void destroy_resource(RefCountedBase*) = 0;
};

// Factory selected at compile time — defined in vk/VulkanBackend.cpp or
// mtl/MetalBackend.mm. Exactly one of these is compiled per build.
Backend* create_backend();

// ─── Helpers used across the lane ───────────────────────────────────────
inline bool has_usage(std::uint32_t mask, BufferUsage u) noexcept {
    return (mask & static_cast<std::uint32_t>(u)) != 0;
}
inline bool has_usage(std::uint32_t mask, TextureUsage u) noexcept {
    return (mask & static_cast<std::uint32_t>(u)) != 0;
}

} // namespace psynder::gpu
