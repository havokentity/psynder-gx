// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/cmd/Cmd.cpp
//
// Anchor for the command-buffer subdirectory. The real recording happens
// in the backend (vk/VulkanCmd.cpp + mtl/MetalCmd.mm). This TU exists so
// the cmd/ directory ships at least one object file and the lane lib has
// a hook for any future API-neutral command helpers (e.g. validation
// passes or debug timestamp insertion).

#include "gpu/PublicGpu.h"
#include "gpu/PublicGpuInternal.h"

namespace psynder::gpu::cmd {

void cmd_anchor() noexcept {}

} // namespace psynder::gpu::cmd
