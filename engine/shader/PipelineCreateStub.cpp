// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/PipelineCreateStub.cpp
//
// Lane 08 — symbol fallback for builds where neither PSYNDER_GX_BACKEND_VULKAN
// nor PSYNDER_GX_BACKEND_METAL is defined (some test-only configurations or
// host-tools builds that only need the slang→SPIR-V text path).
//
// When either backend define is active, this TU compiles empty and the
// real symbols come from PipelineCreateVulkan.cpp / PipelineCreateMetal.mm.
//
// Return-value contract: returning `true` here represents a no-op success
// — "PSO creation isn't applicable to this build configuration, but the
// handle has been allocated and slang→SPIR-V compilation succeeded".  This
// keeps ShaderCompile.cpp from logging "PSO creation failed" against
// host-tools / test-only builds that never had a GPU in the first place.
// Lane 07's bind_pipeline does the right thing on a missing registration
// anyway (logs once + renders blank) — there's no risk in silencing the
// stub.

#include "shader/impl/PipelineCreateBackend.h"

#if !defined(PSYNDER_GX_BACKEND_VULKAN) && !defined(PSYNDER_GX_BACKEND_METAL)

namespace psynder::shader::impl {

bool create_and_register_graphics_pso(
    std::uint32_t,
    const std::vector<std::uint8_t>&,
    const std::vector<std::uint8_t>&,
    const char*,
    const char*,
    const VertexInputDesc&,
    std::uint32_t,
    std::uint8_t,
    bool,
    bool) { return true; } // no-op success — see file header

bool create_and_register_compute_pso(
    std::uint32_t,
    const std::vector<std::uint8_t>&,
    const char*) { return true; }   // no-op success — see file header

void release_registered_pso(std::uint32_t) {}

} // namespace psynder::shader::impl

#endif
