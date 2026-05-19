// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/vk/VulkanBackend.cpp
//
// Vulkan backend for Psynder-GX (Win/Linux).
//
// Compile-gated: only built when PSYNDER_GX_BACKEND_VULKAN is defined
// AND the volk loader is available. On macOS this TU is empty (the lane
// CMakeLists doesn't add it to the source list anyway, but the gate is
// belt-and-braces for header-include slips).
//
// Wave-B M0 goal: instance + physical device + logical device +
// VkSurfaceKHR + VkSwapchainKHR + animated clear color via a render
// pass + dynamic rendering fallback. Per-frame:
//
//   begin_frame:
//     vkAcquireNextImageKHR (signal image-available semaphore)
//   cmd_open:
//     return our recorded command buffer (one per frame)
//   cmd_submit:
//     encode the clear pass against the swapchain image, transition
//     layouts, vkQueueSubmit (wait image-available, signal render-done)
//   end_frame:
//     vkQueuePresentKHR (wait render-done), vkWaitForFences (single
//     buffered)
//
// Win32 surface uses vkCreateWin32SurfaceKHR; Linux uses Wayland with
// xcb fallback. The platform lanes (23 / 24) pass us the appropriate
// native handle via DeviceDesc::native_window_handle.
//
// The platform-handle interpretation:
//   * Win32: native_window_handle is HWND.
//   * Linux Wayland: native_window_handle is wl_surface* (with the
//     compositor accessible via the platform lane's wl_display pointer).
//     We expect a small struct LinuxWaylandHandle behind a tagged
//     interface — TBD with lane 24 via Issue.
//   * Linux X11/xcb: TBD with lane 24.
//
// NO mid-frame allocations. Command pool is per-frame and gets reset
// (not freed) each frame.

#include "gpu/PublicGpu.h"
#include "gpu/PublicGpuInternal.h"

#if !defined(PSYNDER_GX_BACKEND_VULKAN)

// Empty translation unit when Vulkan backend is not enabled. We still
// need an anchor symbol so the static lib has something to link.
namespace psynder::gpu::vk_be {
void vk_anchor_unused() noexcept {}
} // namespace psynder::gpu::vk_be

#else // PSYNDER_GX_BACKEND_VULKAN

#include "gpu/vk/VulkanBackend.h"

#include <volk.h>

#if defined(_WIN32)
#  define VK_USE_PLATFORM_WIN32_KHR 1
#  include <windows.h>
#  include <vulkan/vulkan_win32.h>
#elif defined(__linux__)
#  if defined(PSYNDER_GX_VK_WAYLAND)
#    define VK_USE_PLATFORM_WAYLAND_KHR 1
#    include <wayland-client.h>
#    include <vulkan/vulkan_wayland.h>
#  endif
#  if defined(PSYNDER_GX_VK_XCB)
#    define VK_USE_PLATFORM_XCB_KHR 1
#    include <xcb/xcb.h>
#    include <vulkan/vulkan_xcb.h>
#  endif
#endif

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace psynder::gpu {

namespace vk_be {

namespace {

constexpr std::uint32_t kFramesInFlight = 2;

inline const char* result_str(VkResult r) {
    switch (r) {
        case VK_SUCCESS:               return "VK_SUCCESS";
        case VK_NOT_READY:             return "VK_NOT_READY";
        case VK_TIMEOUT:               return "VK_TIMEOUT";
        case VK_INCOMPLETE:            return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:   return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:     return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_SURFACE_LOST_KHR:return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR:        return "VK_SUBOPTIMAL_KHR";
        default:                       return "VK_???";
    }
}

#define VK_CHECK(expr)                                                     \
    do {                                                                   \
        VkResult vk__r = (expr);                                           \
        if (vk__r != VK_SUCCESS) {                                         \
            std::fprintf(stderr, "[psy::gpu::vk] %s failed: %s\n",         \
                         #expr, result_str(vk__r));                        \
            return false;                                                  \
        }                                                                  \
    } while (0)

} // namespace

class VulkanBackend final : public Backend {
public:
    bool init(Device* dev) override;
    void shutdown(Device* dev) override;

    bool begin_frame(Device* dev) override;
    void end_frame(Device* dev) override;

    CmdBuffer* cmd_open(Device* dev) override;
    void       cmd_submit(Device* dev, CmdBuffer* cmd) override;

    void resize_swapchain(Device* dev, std::uint32_t w, std::uint32_t h) override;

    Buffer*  create_buffer (Device* dev, const BufferDesc& d) override;
    Texture* create_texture(Device* dev, const TextureDesc& d) override;
    Sampler* create_sampler(Device* dev) override;

    void*    buffer_map  (Buffer* b) override;
    void     buffer_unmap(Buffer* b) override;

    void destroy_resource(RefCountedBase* res) override;

private:
    bool create_instance_(Device* dev);
    bool create_surface_(Device* dev, void* native_handle);
    bool select_physical_device_(Device* dev);
    bool create_logical_device_(Device* dev);
    bool create_swapchain_(Device* dev, std::uint32_t w, std::uint32_t h);
    bool create_per_frame_(Device* dev);
    void destroy_swapchain_();
    void destroy_per_frame_();

    VkInstance        instance_       = VK_NULL_HANDLE;
    VkSurfaceKHR      surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice  phys_           = VK_NULL_HANDLE;
    VkDevice          device_         = VK_NULL_HANDLE;
    std::uint32_t     gfx_queue_idx_  = 0;
    VkQueue           gfx_queue_      = VK_NULL_HANDLE;

    VkSwapchainKHR    swapchain_      = VK_NULL_HANDLE;
    VkFormat          sc_format_      = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D        sc_extent_      = {0, 0};
    std::vector<VkImage>     sc_images_;
    std::vector<VkImageView> sc_views_;

    struct PerFrame {
        VkCommandPool   pool      = VK_NULL_HANDLE;
        VkCommandBuffer cb        = VK_NULL_HANDLE;
        VkSemaphore     img_avail = VK_NULL_HANDLE;
        VkSemaphore     rdr_done  = VK_NULL_HANDLE;
        VkFence         in_flight = VK_NULL_HANDLE;
        VkCmdBuf        wrapper   {};
    };
    std::array<PerFrame, kFramesInFlight> frames_{};

    std::uint32_t frame_slot_   = 0;
    std::uint32_t image_index_  = 0;

    char          device_name_buf_[256] = {0};
    bool          surface_attached_     = false;
    bool          swapchain_ready_      = false;
};

Backend* make_vulkan_backend() { return new (std::nothrow) VulkanBackend(); }

// ─── init / shutdown ────────────────────────────────────────────────────
bool VulkanBackend::init(Device* dev) {
    if (volkInitialize() != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] volkInitialize failed (Vulkan loader missing?)\n", stderr);
        return false;
    }
    if (!create_instance_(dev)) return false;

    if (dev->desc.native_window_handle) {
        if (!create_surface_(dev, dev->desc.native_window_handle)) {
            std::fputs("[psy::gpu::vk] surface creation failed\n", stderr);
            // Not fatal in smoke mode; device stays headless.
        } else {
            surface_attached_ = true;
        }
    }

    if (!select_physical_device_(dev)) return false;
    if (!create_logical_device_(dev))  return false;

    if (surface_attached_) {
        // 1280x720 default; resize_swapchain() will rebuild when the
        // platform lane reports a real window size.
        if (!create_swapchain_(dev, 1280, 720))           return false;
        if (!create_per_frame_(dev))                       return false;
        swapchain_ready_ = true;
    }

    std::printf("[psy::gpu::vk] init: device=\"%s\" rt=%d mesh=%d surface=%d swap=%d\n",
                device_name_buf_, (int)dev->supports_rt, (int)dev->supports_mesh,
                (int)surface_attached_, (int)swapchain_ready_);
    return true;
}

void VulkanBackend::shutdown(Device* /*dev*/) {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroy_per_frame_();
        destroy_swapchain_();
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ && instance_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

// ─── Instance ───────────────────────────────────────────────────────────
bool VulkanBackend::create_instance_(Device* dev) {
    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "psynder-gx";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "Psynder-GX";
    app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_3;

    std::vector<const char*> exts;
    exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(_WIN32)
    exts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(__linux__)
#  if defined(VK_USE_PLATFORM_WAYLAND_KHR)
    exts.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#  endif
#  if defined(VK_USE_PLATFORM_XCB_KHR)
    exts.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#  endif
#endif

    std::vector<const char*> layers;
    if (dev->desc.enable_validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = static_cast<std::uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount       = static_cast<std::uint32_t>(layers.size());
    ci.ppEnabledLayerNames     = layers.data();

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    volkLoadInstance(instance_);
    return true;
}

// ─── Surface ────────────────────────────────────────────────────────────
bool VulkanBackend::create_surface_(Device* /*dev*/, void* native_handle) {
#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hwnd      = reinterpret_cast<HWND>(native_handle);
    ci.hinstance = GetModuleHandleW(nullptr);
    VK_CHECK(vkCreateWin32SurfaceKHR(instance_, &ci, nullptr, &surface_));
    return true;
#elif defined(__linux__)
    // The platform-linux lane (24) is expected to give us a small
    // tagged-handle struct so we can pick Wayland vs X11. The contract
    // is TBD via Issue against lane 24; for now we accept a raw
    // wl_surface* under Wayland and a uintptr-encoded xcb_window_t under
    // XCB, selecting by which extension was compiled in.
#  if defined(VK_USE_PLATFORM_WAYLAND_KHR)
    {
        // Lane 24 stub: assume Wayland for now; platform fills out the
        // wl_display pointer via a side channel until the handle struct
        // ABI is agreed.
        (void)native_handle;
        std::fputs("[psy::gpu::vk] Wayland surface creation pending lane-24 handle contract\n", stderr);
        return false;
    }
#  elif defined(VK_USE_PLATFORM_XCB_KHR)
    {
        (void)native_handle;
        std::fputs("[psy::gpu::vk] XCB surface creation pending lane-24 handle contract\n", stderr);
        return false;
    }
#  else
    (void)native_handle;
    std::fputs("[psy::gpu::vk] no Linux WSI extension compiled in\n", stderr);
    return false;
#  endif
#else
    (void)native_handle;
    std::fputs("[psy::gpu::vk] unsupported platform for VkSurfaceKHR\n", stderr);
    return false;
#endif
}

// ─── Physical / logical device ──────────────────────────────────────────
bool VulkanBackend::select_physical_device_(Device* dev) {
    std::uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        std::fputs("[psy::gpu::vk] no Vulkan physical devices found\n", stderr);
        return false;
    }
    std::vector<VkPhysicalDevice> phys(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, phys.data()));

    // Pick first discrete; fall back to first.
    VkPhysicalDevice picked = VK_NULL_HANDLE;
    for (auto p : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            picked = p;
            break;
        }
    }
    if (!picked) picked = phys.front();
    phys_ = picked;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys_, &props);
    std::strncpy(device_name_buf_, props.deviceName, sizeof(device_name_buf_) - 1);
    dev->device_name_cstr = device_name_buf_;

    // Capability detection — extension probing.
    std::uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> ext_props(ext_count);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count, ext_props.data());
    auto has_ext = [&](const char* n) {
        for (auto const& e : ext_props) {
            if (std::strcmp(e.extensionName, n) == 0) return true;
        }
        return false;
    };
    dev->supports_rt   = has_ext(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
                      && has_ext(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    dev->supports_mesh = has_ext(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    // Vulkan only sees host-visible-VRAM as a hint on integrated GPUs.
    // Discrete GPUs report DEVICE_LOCAL ∧ HOST_VISIBLE = false. Apple
    // Silicon is Metal-only.
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys_, &mem);
    bool unified = false;
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        auto flags = mem.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            unified = true;
            break;
        }
    }
    dev->unified_memory = unified;
    return true;
}

bool VulkanBackend::create_logical_device_(Device* dev) {
    // Find a graphics queue family that also supports present (if surface).
    std::uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qf_count, qfs.data());

    std::uint32_t picked_qf = UINT32_MAX;
    for (std::uint32_t i = 0; i < qf_count; ++i) {
        if (!(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (surface_) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys_, i, surface_, &present);
            if (!present) continue;
        }
        picked_qf = i;
        break;
    }
    if (picked_qf == UINT32_MAX) {
        std::fputs("[psy::gpu::vk] no graphics+present queue family found\n", stderr);
        return false;
    }
    gfx_queue_idx_ = picked_qf;

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = picked_qf;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &qprio;

    std::vector<const char*> dexts;
    if (surface_) dexts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // Dynamic rendering — keeps the M0 clear pass renderpass-less.
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext                   = &f13;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = static_cast<std::uint32_t>(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();

    VK_CHECK(vkCreateDevice(phys_, &dci, nullptr, &device_));
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, gfx_queue_idx_, 0, &gfx_queue_);
    (void)dev;
    return true;
}

// ─── Swapchain ──────────────────────────────────────────────────────────
bool VulkanBackend::create_swapchain_(Device* dev, std::uint32_t w, std::uint32_t h) {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));

    // Pick BGRA8_SRGB / BGRA8_UNORM for parity with Metal.
    std::uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fc, fmts.data());

    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts.front();
    for (auto const& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    sc_format_ = chosen.format;

    VkExtent2D extent {
        std::max(caps.minImageExtent.width,  std::min(caps.maxImageExtent.width,  w)),
        std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, h))
    };
    if (extent.width == 0 || extent.height == 0) {
        extent = caps.currentExtent;
    }
    sc_extent_ = extent;
    dev->swapchain_width  = extent.width;
    dev->swapchain_height = extent.height;

    std::uint32_t image_count = std::max(caps.minImageCount + 1, (std::uint32_t)kFramesInFlight);
    if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{};
    sci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface          = surface_;
    sci.minImageCount    = image_count;
    sci.imageFormat      = chosen.format;
    sci.imageColorSpace  = chosen.colorSpace;
    sci.imageExtent      = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode      = VK_PRESENT_MODE_FIFO_KHR; // vsync — competitive FPS users override later
    sci.clipped          = VK_TRUE;
    sci.oldSwapchain     = swapchain_;

    VkSwapchainKHR new_sc = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &new_sc));
    if (swapchain_) {
        for (auto v : sc_views_) vkDestroyImageView(device_, v, nullptr);
        sc_views_.clear();
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }
    swapchain_ = new_sc;

    std::uint32_t cnt = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &cnt, nullptr);
    sc_images_.resize(cnt);
    vkGetSwapchainImagesKHR(device_, swapchain_, &cnt, sc_images_.data());

    sc_views_.resize(cnt);
    for (std::uint32_t i = 0; i < cnt; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image      = sc_images_[i];
        vci.viewType   = VK_IMAGE_VIEW_TYPE_2D;
        vci.format     = sc_format_;
        vci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &sc_views_[i]));
    }
    return true;
}

bool VulkanBackend::create_per_frame_(Device* /*dev*/) {
    for (auto& f : frames_) {
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = gfx_queue_idx_;
        VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &f.pool));

        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = f.pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &f.cb));

        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &f.img_avail));
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &f.rdr_done));

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &f.in_flight));

        f.wrapper.cb   = f.cb;
        f.wrapper.pool = f.pool;
    }
    return true;
}

void VulkanBackend::destroy_per_frame_() {
    for (auto& f : frames_) {
        if (f.in_flight) vkDestroyFence(device_, f.in_flight, nullptr);
        if (f.rdr_done)  vkDestroySemaphore(device_, f.rdr_done,  nullptr);
        if (f.img_avail) vkDestroySemaphore(device_, f.img_avail, nullptr);
        if (f.pool)      vkDestroyCommandPool(device_, f.pool, nullptr);
        // Reset fields explicitly: `f = {}` would copy-assign PerFrame which
        // contains an embedded VkCmdBuf wrapper. VkCmdBuf inherits from
        // CmdBuffer/RefCountedBase whose copy-assignment is deleted, so the
        // implicit PerFrame copy-assign is also deleted. Apple Clang elides
        // the assignment via move-construction; Linux/Windows Clang flag it.
        f.pool      = VK_NULL_HANDLE;
        f.cb        = VK_NULL_HANDLE;
        f.img_avail = VK_NULL_HANDLE;
        f.rdr_done  = VK_NULL_HANDLE;
        f.in_flight = VK_NULL_HANDLE;
        // f.wrapper intentionally left in place — it's a no-op shell that
        // doesn't own any GPU resources itself (the cb above is the owner).
    }
}

void VulkanBackend::destroy_swapchain_() {
    for (auto v : sc_views_) vkDestroyImageView(device_, v, nullptr);
    sc_views_.clear();
    sc_images_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

// ─── Frame loop ─────────────────────────────────────────────────────────
bool VulkanBackend::begin_frame(Device* /*dev*/) {
    if (!swapchain_ready_) return true;

    auto& f = frames_[frame_slot_];
    vkWaitForFences  (device_, 1, &f.in_flight, VK_TRUE, UINT64_MAX);
    vkResetFences    (device_, 1, &f.in_flight);
    vkResetCommandPool(device_, f.pool, 0);

    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       f.img_avail, VK_NULL_HANDLE,
                                       &image_index_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // Caller should call resize_swapchain; skip this frame.
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "[psy::gpu::vk] vkAcquireNextImageKHR: %s\n", result_str(r));
        return false;
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cb, &bi);

    f.wrapper.encoded_clear = false;
    f.wrapper.open       = true;
    f.wrapper.submitted  = false;
    return true;
}

namespace {
void barrier_to_color_attachment(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier b{};
    b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask    = 0;
    b.dstAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.image            = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

void barrier_to_present(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier b{};
    b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask    = 0;
    b.oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout        = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.image            = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}
} // anon

CmdBuffer* VulkanBackend::cmd_open(Device* /*dev*/) {
    if (!swapchain_ready_) return nullptr;
    return &frames_[frame_slot_].wrapper;
}

void VulkanBackend::cmd_submit(Device* dev, CmdBuffer* cmd) {
    if (!swapchain_ready_ || !cmd) return;
    auto& f = frames_[frame_slot_];
    auto* w = static_cast<VkCmdBuf*>(cmd);

    if (!w->encoded_clear) {
        // Encode the animated clear pass.
        // NOTE: we call the CORE Vulkan 1.3 dynamic-rendering entry points
        // (vkCmdBeginRendering, no KHR suffix). The KHR variants are only
        // populated by volk when VK_KHR_dynamic_rendering is listed in the
        // device's ppEnabledExtensionNames — we instead enabled the core
        // 1.3 feature via VkPhysicalDeviceVulkan13Features::dynamicRendering
        // in create_logical_device_(). Calling vkCmdBeginRenderingKHR on a
        // driver that only exposes the core entry (RTX 50-series, latest
        // NVIDIA) crashes (0xC0000005) because the volk table slot is null.
        barrier_to_color_attachment(f.cb, sc_images_[image_index_]);

        VkRenderingAttachmentInfo color{};
        color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView   = sc_views_[image_index_];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

        const double t = (double)dev->current_frame_index * (1.0 / 60.0);
        color.clearValue.color = {{
            (float)(0.5 + 0.5 * std::sin(t * 0.97)),
            (float)(0.5 + 0.5 * std::sin(t * 1.31 + 2.0)),
            (float)(0.5 + 0.5 * std::sin(t * 1.73 + 4.0)),
            1.0f
        }};

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = sc_extent_;
        ri.layerCount           = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments    = &color;
        vkCmdBeginRendering(f.cb, &ri);
        vkCmdEndRendering(f.cb);

        barrier_to_present(f.cb, sc_images_[image_index_]);
        w->encoded_clear = true;
    }

    vkEndCommandBuffer(f.cb);

    VkPipelineStageFlags wait_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &f.img_avail;
    si.pWaitDstStageMask    = &wait_stages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &f.cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &f.rdr_done;
    vkQueueSubmit(gfx_queue_, 1, &si, f.in_flight);
    w->open      = false;
    w->submitted = true;
}

void VulkanBackend::end_frame(Device* dev) {
    if (!swapchain_ready_) return;
    auto& f = frames_[frame_slot_];

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &f.rdr_done;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &swapchain_;
    pi.pImageIndices      = &image_index_;

    VkResult r = vkQueuePresentKHR(gfx_queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        // resize on next frame
        resize_swapchain(dev, dev->swapchain_width, dev->swapchain_height);
    }
    frame_slot_ = (frame_slot_ + 1) % kFramesInFlight;
}

void VulkanBackend::resize_swapchain(Device* dev, std::uint32_t w, std::uint32_t h) {
    if (!swapchain_ready_) return;
    vkDeviceWaitIdle(device_);
    create_swapchain_(dev, w, h);
}

// ─── Resource stubs (M0) ────────────────────────────────────────────────
Buffer*  VulkanBackend::create_buffer (Device*, const BufferDesc&)  { return new (std::nothrow) VkBufferRes(); }
Texture* VulkanBackend::create_texture(Device*, const TextureDesc&) { return new (std::nothrow) VkTextureRes(); }
Sampler* VulkanBackend::create_sampler(Device*)                     { return new (std::nothrow) VkSamplerRes(); }
void*    VulkanBackend::buffer_map  (Buffer* b) { return static_cast<VkBufferRes*>(b)->mapped; }
void     VulkanBackend::buffer_unmap(Buffer*) {}

void VulkanBackend::destroy_resource(RefCountedBase* res) {
    delete res;
}

} // namespace vk_be

// ─── create_backend factory — Vulkan build ──────────────────────────────
Backend* create_backend() {
    return vk_be::make_vulkan_backend();
}

} // namespace psynder::gpu

#endif // PSYNDER_GX_BACKEND_VULKAN
