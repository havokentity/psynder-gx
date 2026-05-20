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
    CmdBuffer* cb = d->backend->cmd_open(d);
    if (cb) {
        // Stamp the owning device so the encoder dispatchers can find
        // the backend without the user threading Device* through every
        // call. Reset transient encoder state on every cmd_open.
        cb->set_owner(d);
        cb->encoded_user_render = false;
        cb->index_buffer        = nullptr;
        cb->index_buffer_offset = 0;
        cb->index_buffer_u32    = 0;
    }
    return cb;
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

// ─── Render-encoder dispatchers ─────────────────────────────────────────
//
// All entry points route through the backend's encoder vtable. Each
// dispatch validates that the CmdBuffer has an owning device (set when
// cmd_open() returned it from the backend) and that the backend pointer
// is live. Once a render pass has begun, cmd_submit() will detect
// cmd->encoded_user_render == true and skip the M0 default auto-clear.

namespace {
inline Backend* backend_for(CmdBuffer* cb) {
    if (!cb || !cb->owner() || !cb->owner()->backend) return nullptr;
    return cb->owner()->backend;
}
} // namespace

void begin_render(CmdBuffer* cb, const RenderPassDesc& desc) {
    auto* be = backend_for(cb);
    if (!be) return;
    cb->encoded_user_render = true; // suppresses M0 auto-clear in cmd_submit
    be->begin_render(cb, desc);
}

void end_render(CmdBuffer* cb) {
    auto* be = backend_for(cb);
    if (!be) return;
    be->end_render(cb);
}

void set_viewport(CmdBuffer* cb, const Viewport& vp) {
    auto* be = backend_for(cb);
    if (!be) return;
    be->set_viewport(cb, vp);
}

void set_scissor(CmdBuffer* cb, const Scissor& sc) {
    auto* be = backend_for(cb);
    if (!be) return;
    be->set_scissor(cb, sc);
}

void bind_pipeline(CmdBuffer* cb, ::psynder::shader::PipelineHandle h) {
    auto* be = backend_for(cb);
    if (!be) return;
    be->bind_pipeline(cb, h);
}

void bind_vertex_buffer(CmdBuffer* cb, std::uint32_t binding,
                        Buffer* buf, std::uint64_t offset) {
    auto* be = backend_for(cb);
    if (!be) return;
    be->bind_vertex_buffer(cb, binding, buf, offset);
}

void bind_index_buffer(CmdBuffer* cb, Buffer* buf,
                       IndexType type, std::uint64_t offset) {
    auto* be = backend_for(cb);
    if (!be) return;
    // Cache on the CmdBuffer so Metal can consume it at draw_indexed time.
    cb->index_buffer        = buf;
    cb->index_buffer_offset = offset;
    cb->index_buffer_u32    = (type == IndexType::U32) ? 1 : 0;
    be->bind_index_buffer(cb, buf, type, offset);
}

void bind_texture(CmdBuffer* cb, std::uint32_t slot, Texture* tex, Sampler* samp) {
    auto* be = backend_for(cb);
    if (!be) return;
    be->bind_texture(cb, slot, tex, samp);
}

void push_constants(CmdBuffer* cb, const void* data,
                    std::uint32_t size, std::uint32_t stage_mask) {
    auto* be = backend_for(cb);
    if (!be) return;
    if (size > kMaxPushConstantBytes) {
        // Hard cap per the public-header contract. Drop the call rather
        // than truncate (truncated state is a worse failure than a
        // visible "no push constants" symptom).
        std::fprintf(stderr,
                     "[psy::gpu] push_constants: size=%u exceeds kMaxPushConstantBytes=%u; dropped\n",
                     size, kMaxPushConstantBytes);
        return;
    }
    be->push_constants(cb, data, size, stage_mask);
}

void draw(CmdBuffer* cb, std::uint32_t vc, std::uint32_t ic,
          std::uint32_t fv, std::uint32_t fi) {
    auto* be = backend_for(cb);
    if (!be) return;
    be->draw(cb, vc, ic, fv, fi);
}

void draw_indexed(CmdBuffer* cb, std::uint32_t ic, std::uint32_t inst,
                  std::uint32_t fi, std::int32_t vo, std::uint32_t fii) {
    auto* be = backend_for(cb);
    if (!be) return;
    be->draw_indexed(cb, ic, inst, fi, vo, fii);
}

void dispatch(CmdBuffer* cb, std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) {
    auto* be = backend_for(cb);
    if (!be) return;
    be->dispatch(cb, gx, gy, gz);
}

} // namespace psynder::gpu
