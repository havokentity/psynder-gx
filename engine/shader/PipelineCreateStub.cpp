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

#include "shader/impl/PipelineCreateBackend.h"

#if !defined(PSYNDER_GX_BACKEND_VULKAN) && !defined(PSYNDER_GX_BACKEND_METAL)

namespace psynder::shader::impl {

bool create_and_register_graphics_pso(
    std::uint32_t,
    const std::vector<std::uint8_t>&,
    const std::vector<std::uint8_t>&,
    const char*,
    const char*) { return false; }

bool create_and_register_compute_pso(
    std::uint32_t,
    const std::vector<std::uint8_t>&,
    const char*) { return false; }

void release_registered_pso(std::uint32_t) {}

} // namespace psynder::shader::impl

#endif
