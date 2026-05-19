// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/vk/VulkanBackend.h
//
// Vulkan backend internal header. Concrete RefCountedBase derivatives
// + the backend class declaration. .cpp under this directory consumes
// volk + vulkan/vulkan.h directly.

#pragma once

#include "gpu/PublicGpu.h"
#include "gpu/PublicGpuInternal.h"

#if defined(PSYNDER_GX_BACKEND_VULKAN)

#include <volk.h>

namespace psynder::gpu {

struct VkBufferRes  : Buffer  { VkBuffer       handle = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* mapped = nullptr; };
struct VkTextureRes : Texture { VkImage        handle = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; };
struct VkSamplerRes : Sampler { VkSampler      handle = VK_NULL_HANDLE; };
struct VkCmdBuf     : CmdBuffer {
    VkCommandBuffer cb     = VK_NULL_HANDLE;
    VkCommandPool   pool   = VK_NULL_HANDLE;
    bool            encoded_clear = false;
};

namespace vk_be {
Backend* make_vulkan_backend();
} // namespace vk_be

} // namespace psynder::gpu

#endif // PSYNDER_GX_BACKEND_VULKAN
