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
// VkTextureRes owns (handle, view, mem) when populated by the M1 upload
// path. `device_for_destroy` is cached at create time so the virtual
// destructor (driven by the deferred-destroy queue in Handle<T>) can free
// the Vulkan objects without a downcast to VulkanBackend* — RTTI is
// avoided per the DOTS rules in AGENTS.md. M0-stub creates leave the
// handle/view/mem null and the dtor short-circuits cleanly.
struct VkTextureRes : Texture {
    VkImage        handle             = VK_NULL_HANDLE;
    VkImageView    view               = VK_NULL_HANDLE;
    VkDeviceMemory mem                = VK_NULL_HANDLE;
    VkDevice       device_for_destroy = VK_NULL_HANDLE;
    ~VkTextureRes() override;
};
struct VkSamplerRes : Sampler { VkSampler      handle = VK_NULL_HANDLE; };
struct VkCmdBuf     : CmdBuffer {
    VkCommandBuffer cb     = VK_NULL_HANDLE;
    VkCommandPool   pool   = VK_NULL_HANDLE;
    bool            encoded_clear = false;
    // Render-encoder bookkeeping. Set in begin_render; consumed by
    // end_render to emit the present-src layout transition exactly when
    // the user rendered into the swapchain image.
    bool            in_render_pass            = false;
    bool            render_pass_to_swapchain  = false;
    VkImage         swapchain_image_for_pass  = VK_NULL_HANDLE;
    // Pipeline-bind state. Cached so push_constants knows the layout
    // (Vulkan requires a VkPipelineLayout argument). Lane 08 publishes
    // both the pipeline + its layout via psynder_gx_vk_register_pipeline.
    VkPipeline       bound_pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout bound_pipeline_layout = VK_NULL_HANDLE;
    VkPipelineBindPoint bound_bind_point  = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

namespace vk_be {
Backend* make_vulkan_backend();
} // namespace vk_be

} // namespace psynder::gpu

#endif // PSYNDER_GX_BACKEND_VULKAN
