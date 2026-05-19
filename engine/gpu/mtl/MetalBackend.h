// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/mtl/MetalBackend.h
//
// Metal backend internal header. Concrete RefCountedBase derivatives
// that carry id<MTLBuffer> / id<MTLTexture> handles, the CAMetalLayer
// pointer, and the per-frame drawable. Included only by .mm files in
// this subdirectory.

#pragma once

#include "gpu/PublicGpu.h"
#include "gpu/PublicGpuInternal.h"

// Per-frame state crosses begin_frame -> cmd_open -> cmd_submit ->
// end_frame. The actual FrameState (carrying Obj-C ids) lives inside
// MetalBackend.mm directly so we never need to forward-declare
// id<T>-typed members in a header where Foundation may not be visible.

namespace psynder::gpu::mtl {

// Forward-declared opaque so the Backend factory can return a non-Obj-C
// symbol from a plain C++ translation unit.
class MetalBackend;

Backend* make_metal_backend();

} // namespace psynder::gpu::mtl
