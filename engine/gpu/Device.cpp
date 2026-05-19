// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/Device.cpp
//
// Common dispatcher for the PublicGpu.h surface. Selects the backend
// (Vulkan on Win/Linux, Metal on macOS) at compile time via the
// PSYNDER_GX_BACKEND_* defines that the lane CMakeLists wires up.
//
// All entry points are thin wrappers around the Backend dispatch table.
// The expensive work lives in the backend impls under vk/ and mtl/.

#include "gpu/PublicGpu.h"
#include "gpu/PublicGpuInternal.h"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace psynder::gpu {

// ─── Device lifetime ────────────────────────────────────────────────────
Device* create_device(const DeviceDesc& desc) {
    auto* dev = new (std::nothrow) Device{};
    if (!dev) {
        std::fputs("[psy::gpu] create_device: out of memory allocating Device\n", stderr);
        return nullptr;
    }
    dev->desc = desc;

    dev->backend = create_backend();
    if (!dev->backend) {
        std::fputs("[psy::gpu] create_device: no backend compiled into this build\n", stderr);
        delete dev;
        return nullptr;
    }

    if (!dev->backend->init(dev)) {
        std::fputs("[psy::gpu] create_device: backend init failed\n", stderr);
        delete dev->backend;
        delete dev;
        return nullptr;
    }
    return dev;
}

void destroy_device(Device* dev) {
    if (!dev) return;
    // Contract: callers must drop all Handle<T> referencing GPU resources
    // belonging to this device BEFORE calling destroy_device. Failure to
    // do so leaks the underlying API handle (Metal id<MTLBuffer> etc.)
    // because the deferred-destroy path through dev->backend is torn
    // down here. We don't try to walk a live-resource list at shutdown —
    // the engine is monolithic, levels unload by reference-dropping
    // their owned Handles in scene tear-down (lane 06).
    if (dev->backend) {
        dev->backend->shutdown(dev);
        delete dev->backend;
        dev->backend = nullptr;
    }
    delete dev;
}

bool device_supports_rt           (const Device* d) { return d && d->supports_rt; }
bool device_supports_mesh_shaders (const Device* d) { return d && d->supports_mesh; }
bool device_is_unified_memory     (const Device* d) { return d && d->unified_memory; }
const char* device_name           (const Device* d) { return d ? d->device_name_cstr : ""; }

// ─── Frame loop ─────────────────────────────────────────────────────────
bool begin_frame(Device* d) {
    if (!d || !d->backend) return false;
    ++d->current_frame_index;
    return d->backend->begin_frame(d);
}

void end_frame(Device* d) {
    if (!d || !d->backend) return;
    d->backend->end_frame(d);
    // Naive completion model: after end_frame, the previous frame has
    // been retired by the backend's wait-on-fence/drawable. This will
    // get tightened when frames-in-flight > 1 lands.
    d->last_completed_frame = d->current_frame_index;
}

CmdBuffer* cmd_open(Device* d) {
    if (!d || !d->backend) return nullptr;
    return d->backend->cmd_open(d);
}

void cmd_submit(Device* d, CmdBuffer* cb) {
    if (!d || !d->backend || !cb) return;
    d->backend->cmd_submit(d, cb);
}

void resize_swapchain(Device* d, std::uint32_t w, std::uint32_t h) {
    if (!d || !d->backend) return;
    d->backend->resize_swapchain(d, w, h);
}

// ─── Resource creation (stubbed for M0; Buffer/Texture impl is M1) ──────
Handle<Buffer> create_buffer(Device* d, const BufferDesc& desc) {
    if (!d || !d->backend) return Handle<Buffer>{};
    Buffer* b = d->backend->create_buffer(d, desc);
    if (!b) return Handle<Buffer>{};
    b->set_owner(d);
    b->desc = desc;
    return Handle<Buffer>{b};
}

Handle<Texture> create_texture(Device* d, const TextureDesc& desc) {
    if (!d || !d->backend) return Handle<Texture>{};
    Texture* t = d->backend->create_texture(d, desc);
    if (!t) return Handle<Texture>{};
    t->set_owner(d);
    t->desc = desc;
    return Handle<Texture>{t};
}

Handle<Sampler> create_sampler(Device* d) {
    if (!d || !d->backend) return Handle<Sampler>{};
    Sampler* s = d->backend->create_sampler(d);
    if (!s) return Handle<Sampler>{};
    s->set_owner(d);
    return Handle<Sampler>{s};
}

void* buffer_map(Buffer* b) {
    if (!b || !b->owner() || !b->owner()->backend) return nullptr;
    return b->owner()->backend->buffer_map(b);
}

void buffer_unmap(Buffer* b) {
    if (!b || !b->owner() || !b->owner()->backend) return;
    b->owner()->backend->buffer_unmap(b);
}

// ─── RT (M5 — stubbed) ──────────────────────────────────────────────────
Handle<AccelerationStructure> create_blas(Device* d, const BlasDesc& desc) {
    if (!d || !d->backend) return Handle<AccelerationStructure>{};
    auto* a = d->backend->create_blas(d, desc);
    if (!a) return Handle<AccelerationStructure>{};
    a->set_owner(d);
    return Handle<AccelerationStructure>{a};
}

Handle<AccelerationStructure> create_tlas(Device* d, const TlasDesc& desc) {
    if (!d || !d->backend) return Handle<AccelerationStructure>{};
    auto* a = d->backend->create_tlas(d, desc);
    if (!a) return Handle<AccelerationStructure>{};
    a->set_owner(d);
    return Handle<AccelerationStructure>{a};
}

void refit_tlas(Device* d, AccelerationStructure* a) {
    if (!d || !d->backend) return;
    d->backend->refit_tlas(d, a);
}

// ─── BlasDesc / TlasDesc dummy definitions ──────────────────────────────
//
// PublicGpu.h forward-declares these. M5 will give them real bodies; for
// now we just provide empty types so the headers link. Render-RT lane
// (lane 10) declares them privately at the call site for its own M5
// scaffolding — the contract is "this is opaque at M0".
//
// NOTE: this is intentionally NOT in PublicGpu.h (which is frozen). The
// types are forward-declared there and given empty bodies inline here.
// When lane 10 ships, it'll fill these in by filing an Issue for an
// agreed-upon contract change.

} // namespace psynder::gpu
