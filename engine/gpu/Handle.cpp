// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/Handle.cpp
//
// Out-of-line definitions for psy::gpu::Handle<T>. The public header
// (PublicGpu.h) declares the special members but cannot define them —
// they need to see the RefCountedBase ABI to manipulate refcounts.
//
// We define them here as inline-template bodies inside the lane, then
// EXPLICITLY INSTANTIATE for the concrete types that the PublicGpu.h
// surface returns (Buffer, Texture, Sampler, AccelerationStructure).
// Other lanes link against the static lib that carries the
// instantiations, so they never need to see RefCountedBase.

#include "gpu/PublicGpuInternal.h"

namespace psynder::gpu {

// ─── Out-of-line template definitions ───────────────────────────────────
//
// These are emitted once per explicit instantiation below. Each Handle<T>
// pumps T's refcount via the RefCountedBase base class.

template <typename T>
Handle<T>::Handle(T* p) noexcept : ptr_(p) {
    if (ptr_) {
        static_cast<RefCountedBase*>(ptr_)->add_ref();
    }
}

template <typename T>
Handle<T>::Handle(const Handle& o) noexcept : ptr_(o.ptr_) {
    if (ptr_) {
        static_cast<RefCountedBase*>(ptr_)->add_ref();
    }
}

template <typename T>
Handle<T>::Handle(Handle&& o) noexcept : ptr_(o.ptr_) {
    o.ptr_ = nullptr;
}

template <typename T>
Handle<T>& Handle<T>::operator=(const Handle& o) noexcept {
    if (this == &o) {
        return *this;
    }
    if (o.ptr_) {
        static_cast<RefCountedBase*>(o.ptr_)->add_ref();
    }
    T* prev = ptr_;
    ptr_ = o.ptr_;
    if (prev) {
        auto* base = static_cast<RefCountedBase*>(prev);
        if (base->release_ref() && base->owner() && base->owner()->backend) {
            base->owner()->backend->destroy_resource(base);
        }
    }
    return *this;
}

template <typename T>
Handle<T>& Handle<T>::operator=(Handle&& o) noexcept {
    if (this == &o) {
        return *this;
    }
    T* prev = ptr_;
    ptr_   = o.ptr_;
    o.ptr_ = nullptr;
    if (prev) {
        auto* base = static_cast<RefCountedBase*>(prev);
        if (base->release_ref() && base->owner() && base->owner()->backend) {
            base->owner()->backend->destroy_resource(base);
        }
    }
    return *this;
}

template <typename T>
Handle<T>::~Handle() {
    if (!ptr_) {
        return;
    }
    auto* base = static_cast<RefCountedBase*>(ptr_);
    if (base->release_ref() && base->owner() && base->owner()->backend) {
        // Backend is responsible for honoring retire_frame before freeing.
        base->owner()->backend->destroy_resource(base);
    }
}

// ─── Explicit instantiations — these are the only public-surface types ──
template class Handle<Buffer>;
template class Handle<Texture>;
template class Handle<Sampler>;
template class Handle<AccelerationStructure>;
template class Handle<Pipeline>;
template class Handle<DescriptorSet>;
template class Handle<Heap>;

} // namespace psynder::gpu
