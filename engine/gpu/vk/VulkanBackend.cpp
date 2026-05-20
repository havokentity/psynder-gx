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
// The platform-handle interpretation (current — Issue lane09-002 resolved):
//   * Win32: native_window_handle is HWND (void* cast).
//   * macOS: native_window_handle is CAMetalLayer* (void* cast) —
//     handled by the Metal backend, never reaches this TU.
//   * Linux: native_window_handle is psynder::gpu::LinuxNativeWindowHandle*
//     (void* cast). The struct carries a Kind tag (Invalid / Wayland / Xcb)
//     plus a union of OS-specific sub-fields stored as void*/uint32 so
//     wayland-client.h and xcb/xcb.h stay out of PublicGpu.h. We cast
//     them back to concrete types in create_surface_() below where the
//     OS headers are already included.
//
// NO mid-frame allocations. Command pool is per-frame and gets reset
// (not freed) each frame.

#include "gpu/PublicGpu.h"
#include "gpu/PublicGpuInternal.h"

#if defined(__linux__) && !defined(PSYNDER_GX_DEDICATED_SERVER)
// LinuxNativeWindowHandle is defined in PublicGpu.h (already included above).
// This comment is the only Linux-specific include needed here — the wl_display,
// wl_surface, xcb_connection_t types are accessed via void* casts and the
// platform headers (wayland-client.h / xcb/xcb.h) are pulled in below inside
// the platform guards.
#endif

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
#include <mutex>
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

    // Render-encoder API (lane09-001 unblock).
    void begin_render(CmdBuffer*, const RenderPassDesc&) override;
    void end_render  (CmdBuffer*) override;
    void set_viewport(CmdBuffer*, const Viewport&) override;
    void set_scissor (CmdBuffer*, const Scissor&)  override;
    void bind_pipeline     (CmdBuffer*, ::psynder::shader::PipelineHandle) override;
    void bind_vertex_buffer(CmdBuffer*, std::uint32_t, Buffer*, std::uint64_t) override;
    void bind_index_buffer (CmdBuffer*, Buffer*, IndexType, std::uint64_t) override;
    void bind_texture      (CmdBuffer*, std::uint32_t, Texture*, Sampler*) override;
    void push_constants    (CmdBuffer*, const void*, std::uint32_t, std::uint32_t) override;
    void draw        (CmdBuffer*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override;
    void draw_indexed(CmdBuffer*, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t) override;
    void dispatch    (CmdBuffer*, std::uint32_t, std::uint32_t, std::uint32_t) override;

    void destroy_resource(RefCountedBase* res) override;

    // Test-only readback (see PublicGpuInternal.h Backend::texture_readback_mip0).
    bool texture_readback_mip0(Texture* tex, void* out_dst_bytes,
                               std::size_t dst_bytes_size) override;

private:
    bool create_instance_(Device* dev);
    bool create_surface_(Device* dev, void* native_handle);
    bool select_physical_device_(Device* dev);
    bool create_logical_device_(Device* dev);
    bool create_swapchain_(Device* dev, std::uint32_t w, std::uint32_t h);
    bool create_per_frame_(Device* dev);
    void destroy_swapchain_();
    void destroy_per_frame_();
    // Deferred-destroy: free every queued resource whose last-live frame is
    // <= through_frame (the GPU has retired it). Pass UINT64_MAX after a
    // vkDeviceWaitIdle to drain unconditionally.
    void reclaim_retired_(std::uint64_t through_frame);
    // Destroy the VkPipelines + VkPipelineLayouts lane 08 registered into the
    // bind shim. Called from shutdown() once the GPU is idle.
    void destroy_registered_pipelines_();

    VkInstance        instance_       = VK_NULL_HANDLE;
    VkSurfaceKHR      surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice  phys_           = VK_NULL_HANDLE;
    VkDevice          device_         = VK_NULL_HANDLE;
    std::uint32_t     gfx_queue_idx_  = 0;
    VkQueue           gfx_queue_      = VK_NULL_HANDLE;

    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    bool                     validation_on_   = false;

    VkSwapchainKHR    swapchain_      = VK_NULL_HANDLE;
    VkFormat          sc_format_      = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D        sc_extent_      = {0, 0};
    std::vector<VkImage>     sc_images_;
    std::vector<VkImageView> sc_views_;
    // Render-finished semaphore is PER SWAPCHAIN IMAGE (not per frame-in-
    // flight): vkQueuePresentKHR(image i) waits on the semaphore the submit for
    // image i signalled, and that semaphore must not be reused until that
    // present completes. vkAcquireNextImageKHR won't return an image whose
    // present is still pending, so indexing by image index is the safe,
    // canonical pattern (a per-frame render-finished semaphore has a reuse
    // hazard once swapchain image count > frames-in-flight).
    std::vector<VkSemaphore> sc_render_done_;

    struct PerFrame {
        VkCommandPool   pool      = VK_NULL_HANDLE;
        VkCommandBuffer cb        = VK_NULL_HANDLE;
        VkSemaphore     img_avail = VK_NULL_HANDLE;
        VkFence         in_flight = VK_NULL_HANDLE;
        VkCmdBuf        wrapper   {};
    };
    std::array<PerFrame, kFramesInFlight> frames_{};

    std::uint32_t frame_slot_   = 0;
    std::uint32_t image_index_  = 0;

    // ─── Deferred-destroy queue ─────────────────────────────────────────────
    // destroy_resource() enqueues here tagged with the frame index in which
    // the resource was last live, instead of deleting inline (the GPU may
    // still be reading it from an in-flight command buffer). Entries are
    // reclaimed once the owning frame has retired — proven by the per-slot
    // fence wait in begin_frame, or a vkDeviceWaitIdle on resize/shutdown.
    // Mutex-guarded: Handle<T> may drop its last reference from a JobSystem
    // worker thread, off the render thread.
    struct PendingDestroy { RefCountedBase* res = nullptr; std::uint64_t frame = 0; };
    std::vector<PendingDestroy> pending_destroy_;
    std::mutex                  pending_destroy_mu_;
    // Frame index of the submission that last used each in-flight slot; read
    // back after that slot's fence signals to advance gpu_completed_frame_.
    std::array<std::uint64_t, kFramesInFlight> slot_frame_{};
    std::uint64_t               gpu_completed_frame_ = 0;

    char          device_name_buf_[256] = {0};
    bool          surface_attached_     = false;
    bool          swapchain_ready_      = false;
    bool          device_lost_          = false;
};

Backend* make_vulkan_backend() { return new (std::nothrow) VulkanBackend(); }

// ─── M1 texture descriptor infra (lane 07-owned) ────────────────────────────
// The M1 textured-triangle fragment shader samples one texture + sampler
// (set 0: binding 0 = sampled image, binding 1 = sampler). Lane 07 owns the
// matching descriptor-set layout + a small pool; lane 08 reuses the same layout
// for its pipeline layout via psynder_gx_vk_m1_texture_set_layout() (identically
// defined set layouts are Vulkan-compatible, so the set allocated here binds
// cleanly to lane 08's pipeline). Process-global like the pipeline shim
// (single-device assumption). M2's reflected, multi-set allocator supersedes it.
namespace {

VkDevice              g_desc_device   = VK_NULL_HANDLE;
VkDescriptorSetLayout g_m1_set_layout = VK_NULL_HANDLE;
VkDescriptorPool      g_desc_pool     = VK_NULL_HANDLE;

struct TexSetEntry { VkImageView view; VkSampler smp; VkDescriptorSet set; };
constexpr std::uint32_t kMaxTexSets = 64;
TexSetEntry   g_tex_sets[kMaxTexSets] = {};
std::uint32_t g_tex_set_count = 0;

void ensure_m1_descriptor_infra(VkDevice dev) {
    if (g_desc_device == dev && g_m1_set_layout != VK_NULL_HANDLE) return;
    g_desc_device = dev;

    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 2; lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lci, nullptr, &g_m1_set_layout) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] m1 descriptor set layout creation failed\n", stderr);
        g_m1_set_layout = VK_NULL_HANDLE;
        return;
    }
    VkDescriptorPoolSize ps[2] = {};
    ps[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; ps[0].descriptorCount = kMaxTexSets;
    ps[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;       ps[1].descriptorCount = kMaxTexSets;
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = kMaxTexSets; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &g_desc_pool) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] m1 descriptor pool creation failed\n", stderr);
        vkDestroyDescriptorSetLayout(dev, g_m1_set_layout, nullptr);
        g_m1_set_layout = VK_NULL_HANDLE;
    }
}

void destroy_m1_descriptor_infra() {
    if (g_desc_device == VK_NULL_HANDLE) return;
    if (g_desc_pool)     vkDestroyDescriptorPool(g_desc_device, g_desc_pool, nullptr);
    if (g_m1_set_layout) vkDestroyDescriptorSetLayout(g_desc_device, g_m1_set_layout, nullptr);
    g_desc_pool = VK_NULL_HANDLE; g_m1_set_layout = VK_NULL_HANDLE; g_desc_device = VK_NULL_HANDLE;
    g_tex_set_count = 0;
    for (auto& e : g_tex_sets) e = {};
}

// Allocate-or-reuse a descriptor set for (view, sampler). The M1 texture is
// static, so a set is written once and re-bound each frame (no in-flight
// descriptor update). Returns VK_NULL_HANDLE if the infra isn't ready.
VkDescriptorSet m1_tex_descriptor_set(VkDevice dev, VkImageView view, VkSampler smp) {
    if (g_m1_set_layout == VK_NULL_HANDLE || g_desc_pool == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    for (std::uint32_t i = 0; i < g_tex_set_count; ++i)
        if (g_tex_sets[i].view == view && g_tex_sets[i].smp == smp) return g_tex_sets[i].set;
    if (g_tex_set_count >= kMaxTexSets) return VK_NULL_HANDLE;

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = g_desc_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_m1_set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkDescriptorImageInfo img{}; img.imageView = view; img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo smi{}; smi.sampler = smp;
    VkWriteDescriptorSet w[2] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = set; w[0].dstBinding = 0;
    w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[0].pImageInfo = &img;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = set; w[1].dstBinding = 1;
    w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; w[1].pImageInfo = &smi;
    vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);

    g_tex_sets[g_tex_set_count++] = { view, smp, set };
    return set;
}

} // anonymous

// Lane 08 reuses this exact set layout for its graphics pipeline layout (see
// the M1 descriptor-infra note above). Returns null until the device is up.
extern "C" void* psynder_gx_vk_m1_texture_set_layout() {
    return reinterpret_cast<void*>(g_m1_set_layout);
}

// Lane 08 (shader) builds its deferred VkPipelines from this context hook
// (defined in engine/shader/PipelineCreateVulkan.cpp). Publishing the device +
// swapchain format here drains lane 08's deferred-pipeline queue so
// bind_pipeline can resolve a PipelineHandle — without it every draw no-ops.
// extern "C" so we don't pull in the shader lane's headers. (This is the
// "Lane 07 will gain a one-line call into this" follow-up noted in that file.)
extern "C" void psynder_gx_shader_push_vk_context(void* vk_device,
                                                  void* vk_physical_device,
                                                  std::uint32_t swapchain_color_format);

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

    // Build the M1 texture set layout BEFORE handing lane 08 the device:
    // its pipeline layouts query psynder_gx_vk_m1_texture_set_layout() while
    // draining the deferred-pipeline queue inside the push below.
    ensure_m1_descriptor_infra(device_);

    // Hand lane 08 the live device + swapchain color format so its deferred
    // graphics/compute pipelines build and register into our bind-shim.
    psynder_gx_shader_push_vk_context(
        reinterpret_cast<void*>(device_),
        reinterpret_cast<void*>(phys_),
        static_cast<std::uint32_t>(sc_format_));

    std::printf("[psy::gpu::vk] init: device=\"%s\" rt=%d mesh=%d surface=%d swap=%d\n",
                device_name_buf_, (int)dev->supports_rt, (int)dev->supports_mesh,
                (int)surface_attached_, (int)swapchain_ready_);
    return true;
}

void VulkanBackend::shutdown(Device* /*dev*/) {
    if (device_) {
        vkDeviceWaitIdle(device_);
        // GPU is idle — tear down every device child BEFORE vkDestroyDevice or
        // the validation layer flags leaks. Order matters: free the descriptor
        // pool (its sets reference the M1 sampler + image views) before
        // reclaiming the deferred-destroy resources that own those objects.
        destroy_registered_pipelines_();   // lane-08 VkPipeline + VkPipelineLayout
        destroy_m1_descriptor_infra();      // descriptor pool + sets + set layout
        reclaim_retired_(UINT64_MAX);        // buffers / textures / samplers
        destroy_per_frame_();
        destroy_swapchain_();
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ && instance_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (debug_messenger_ && instance_) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

namespace {
// Validation / debug-utils message sink. Routes layer messages to stderr with
// a severity tag; returns VK_FALSE so the offending call still proceeds (we
// want the VkResult too, not an abort). Used both as the vkCreateInstance
// pNext (captures create/destroy-time messages) and as the persistent
// messenger.
VKAPI_ATTR VkBool32 VKAPI_CALL debug_utils_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/) {
    const char* sev =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? "ERROR" :
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN"  : "INFO";
    std::fprintf(stderr, "[vk-validation %s] %s\n",
                 sev, (data && data->pMessage) ? data->pMessage : "(no message)");
    return VK_FALSE;
}

bool instance_layer_available(const char* name) {
    std::uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> props(n);
    if (n) vkEnumerateInstanceLayerProperties(&n, props.data());
    for (auto const& p : props) if (std::strcmp(p.layerName, name) == 0) return true;
    return false;
}

void fill_debug_messenger_ci(VkDebugUtilsMessengerCreateInfoEXT& ci) {
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                   | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                   | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debug_utils_callback;
}
} // anonymous

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

    // Validation: on when the app asks (DeviceDesc.enable_validation) or the
    // PSYNDER_GX_VK_VALIDATION env var is set — the env var lets us flip it on
    // a release build with no recompile. Only enable if the layer is actually
    // installed (Vulkan SDK); otherwise warn and run unvalidated.
    std::vector<const char*> layers;
    const bool want_validation =
        dev->desc.enable_validation || (std::getenv("PSYNDER_GX_VK_VALIDATION") != nullptr);
    if (want_validation) {
        if (instance_layer_available("VK_LAYER_KHRONOS_validation")) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            validation_on_ = true;
        } else {
            std::fputs("[psy::gpu::vk] validation requested but VK_LAYER_KHRONOS_validation "
                       "is not installed (need the Vulkan SDK); continuing without it\n", stderr);
        }
    }

    VkDebugUtilsMessengerCreateInfoEXT dbg_ci{};
    if (validation_on_) fill_debug_messenger_ci(dbg_ci);

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = static_cast<std::uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount       = static_cast<std::uint32_t>(layers.size());
    ci.ppEnabledLayerNames     = layers.data();
    // Chain the messenger CI so messages emitted during vkCreateInstance /
    // vkDestroyInstance are captured too.
    ci.pNext                   = validation_on_ ? &dbg_ci : nullptr;

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    volkLoadInstance(instance_);

    // Persistent messenger for the rest of the instance lifetime.
    if (validation_on_) {
        if (vkCreateDebugUtilsMessengerEXT(instance_, &dbg_ci, nullptr, &debug_messenger_) != VK_SUCCESS) {
            std::fputs("[psy::gpu::vk] vkCreateDebugUtilsMessengerEXT failed; validation is on "
                       "but messages won't be routed\n", stderr);
            debug_messenger_ = VK_NULL_HANDLE;
        } else {
            std::fputs("[psy::gpu::vk] validation layer + debug messenger ENABLED\n", stderr);
        }
    }
    return true;
}

// ─── Surface ────────────────────────────────────────────────────────────
//
// Platform-handle interpretation:
//   Win32  — native_handle is HWND (void* cast).
//   macOS  — Metal backend; this function is never called on macOS.
//   Linux  — native_handle is LinuxNativeWindowHandle* (void* cast).
//             The struct carries a Kind tag (Wayland / Xcb) plus the
//             OS-specific sub-fields stored as void* to avoid pulling
//             Wayland/XCB headers into PublicGpu.h.  We cast the void*
//             sub-fields back to their concrete types here where the
//             platform headers are already included.
bool VulkanBackend::create_surface_(Device* /*dev*/, void* native_handle) {
#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hwnd      = reinterpret_cast<HWND>(native_handle);
    ci.hinstance = GetModuleHandleW(nullptr);
    VK_CHECK(vkCreateWin32SurfaceKHR(instance_, &ci, nullptr, &surface_));
    return true;
#elif defined(__linux__)
    // Interpret native_handle as the tagged LinuxNativeWindowHandle struct
    // populated by lane 24's create_window_impl (LinuxPlatform.cpp).
    // The void*-typed sub-fields are cast back to their concrete types here;
    // wayland-client.h and xcb/xcb.h are already included above.
    if (!native_handle) {
        std::fputs("[psy::gpu::vk] native_handle is null on Linux\n", stderr);
        return false;
    }
    const auto* lh = reinterpret_cast<const psynder::gpu::LinuxNativeWindowHandle*>(
        native_handle);

    switch (lh->kind) {
#  if defined(VK_USE_PLATFORM_WAYLAND_KHR)
    case psynder::gpu::LinuxNativeWindowHandle::Kind::Wayland: {
        // wl_display* and wl_surface* were stored as void* in the tagged
        // struct (PublicGpu.h) to avoid pulling wayland-client.h into the
        // public GPU header.  Cast them back here where the header is present.
        auto* display = reinterpret_cast<wl_display*>(lh->wayland.wl_display);
        auto* surface = reinterpret_cast<wl_surface*>(lh->wayland.wl_surface);
        if (!display || !surface) {
            std::fputs("[psy::gpu::vk] Wayland handle has null display or surface\n",
                       stderr);
            return false;
        }
        VkWaylandSurfaceCreateInfoKHR ci{};
        ci.sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        ci.display = display;
        ci.surface = surface;
        VK_CHECK(vkCreateWaylandSurfaceKHR(instance_, &ci, nullptr, &surface_));
        return true;
    }
#  endif // VK_USE_PLATFORM_WAYLAND_KHR

#  if defined(VK_USE_PLATFORM_XCB_KHR)
    case psynder::gpu::LinuxNativeWindowHandle::Kind::Xcb: {
        // xcb_connection_t* and xcb_window_t (uint32) were stored as
        // void* / uint32 in the tagged struct.  Cast back here where
        // xcb/xcb.h is present.
        auto* conn   = reinterpret_cast<xcb_connection_t*>(lh->xcb.xcb_connection);
        auto  window = static_cast<xcb_window_t>(lh->xcb.xcb_window);
        if (!conn || window == 0) {
            std::fputs("[psy::gpu::vk] XCB handle has null connection or zero window\n",
                       stderr);
            return false;
        }
        VkXcbSurfaceCreateInfoKHR ci{};
        ci.sType      = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        ci.connection = conn;
        ci.window     = window;
        VK_CHECK(vkCreateXcbSurfaceKHR(instance_, &ci, nullptr, &surface_));
        return true;
    }
#  endif // VK_USE_PLATFORM_XCB_KHR

    // Two distinct failure modes folded into the same arm so we report
    // accurately. Kind::Invalid means the caller never populated the
    // handle. A valid Kind value (Wayland/Xcb) landing here means the
    // corresponding VK_USE_PLATFORM_*_KHR was not defined at compile time
    // — different fix from the user (rebuild) vs different fix from the
    // caller (populate the handle).
    case psynder::gpu::LinuxNativeWindowHandle::Kind::Invalid:
        std::fputs("[psy::gpu::vk] LinuxNativeWindowHandle is Kind::Invalid "
                   "(default-constructed?); the platform lane must populate "
                   "kind + the active union variant before psy::gpu::create_device()\n",
                   stderr);
        return false;
    default: {
        const int k = static_cast<int>(lh->kind);
#  if !defined(VK_USE_PLATFORM_WAYLAND_KHR) && !defined(VK_USE_PLATFORM_XCB_KHR)
        std::fprintf(stderr,
            "[psy::gpu::vk] LinuxNativeWindowHandle::Kind = %d but this build "
            "has neither VK_USE_PLATFORM_WAYLAND_KHR nor VK_USE_PLATFORM_XCB_KHR "
            "defined — rebuild psynder_gpu with the relevant CMake flags\n", k);
#  elif !defined(VK_USE_PLATFORM_WAYLAND_KHR)
        std::fprintf(stderr,
            "[psy::gpu::vk] LinuxNativeWindowHandle::Kind = %d (Wayland?) but "
            "VK_USE_PLATFORM_WAYLAND_KHR not defined at compile time; either "
            "rebuild with the Wayland WSI flag or use Kind::Xcb\n", k);
#  elif !defined(VK_USE_PLATFORM_XCB_KHR)
        std::fprintf(stderr,
            "[psy::gpu::vk] LinuxNativeWindowHandle::Kind = %d (Xcb?) but "
            "VK_USE_PLATFORM_XCB_KHR not defined at compile time; either "
            "rebuild with the XCB WSI flag or use Kind::Wayland\n", k);
#  else
        std::fprintf(stderr,
            "[psy::gpu::vk] LinuxNativeWindowHandle::Kind = %d is outside "
            "the known enum range — check the platform lane for ABI drift\n", k);
#  endif
        return false;
    }
    }
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

    // The surface dictates the extent on Win32 / Wayland: when currentExtent is
    // not the 0xFFFFFFFF sentinel we MUST use it verbatim (creating the
    // swapchain at any other size is a validation error / undefined). Only when
    // the surface defers to the app (sentinel) do we clamp the requested size.
    VkExtent2D extent;
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        extent = caps.currentExtent;
    } else {
        extent.width  = std::max(caps.minImageExtent.width,  std::min(caps.maxImageExtent.width,  w));
        extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, h));
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

    // One render-finished semaphore per swapchain image (see member comment).
    // Recreated here because the image count can change across a resize; the
    // caller (resize_swapchain) has already vkDeviceWaitIdle'd, so destroying
    // the old set is safe.
    for (auto s : sc_render_done_) vkDestroySemaphore(device_, s, nullptr);
    sc_render_done_.assign(cnt, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < cnt; ++i) {
        VkSemaphoreCreateInfo ssci{};
        ssci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(device_, &ssci, nullptr, &sc_render_done_[i]));
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
        // Render-finished semaphore is per-swapchain-image (create_swapchain_).

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
        f.in_flight = VK_NULL_HANDLE;
        // f.wrapper intentionally left in place — it's a no-op shell that
        // doesn't own any GPU resources itself (the cb above is the owner).
    }
}

void VulkanBackend::destroy_swapchain_() {
    for (auto s : sc_render_done_) vkDestroySemaphore(device_, s, nullptr);
    sc_render_done_.clear();
    for (auto v : sc_views_) vkDestroyImageView(device_, v, nullptr);
    sc_views_.clear();
    sc_images_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

// ─── Frame loop ─────────────────────────────────────────────────────────
bool VulkanBackend::begin_frame(Device* dev) {
    if (device_lost_)      return false;
    if (!swapchain_ready_) return true;

    auto& f = frames_[frame_slot_];
    vkWaitForFences(device_, 1, &f.in_flight, VK_TRUE, UINT64_MAX);

    // The slot's fence is signalled ⇒ the submission that last used this slot
    // has retired on the GPU. Advance the completed-frame watermark and reclaim
    // any deferred-destroy resources whose last-live frame is now behind it.
    if (slot_frame_[frame_slot_] > gpu_completed_frame_)
        gpu_completed_frame_ = slot_frame_[frame_slot_];
    reclaim_retired_(gpu_completed_frame_);

    // Acquire BEFORE resetting the fence. If the acquire fails (OUT_OF_DATE on
    // a resize), we bail without having reset the fence — so it stays signalled
    // and the next begin_frame on this slot won't deadlock in vkWaitForFences.
    // (A signalled fence + early-out was the original hang-on-resize bug.)
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       f.img_avail, VK_NULL_HANDLE,
                                       &image_index_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // Surface no longer matches the swapchain; rebuild and skip this frame.
        // The acquire failed, so img_avail was not signalled and is reusable.
        resize_swapchain(dev, dev->swapchain_width, dev->swapchain_height);
        return false;
    }
    if (r == VK_ERROR_DEVICE_LOST) {
        std::fputs("[psy::gpu::vk] vkAcquireNextImageKHR: VK_ERROR_DEVICE_LOST — halting render\n", stderr);
        device_lost_ = true;
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "[psy::gpu::vk] vkAcquireNextImageKHR: %s\n", result_str(r));
        return false;
    }

    // Acquire succeeded — this frame WILL submit + signal the fence, so it is
    // now safe to reset it.
    vkResetFences(device_, 1, &f.in_flight);
    vkResetCommandPool(device_, f.pool, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(f.cb, &bi) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] vkBeginCommandBuffer failed\n", stderr);
        return false;
    }

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

    if (vkEndCommandBuffer(f.cb) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] vkEndCommandBuffer failed\n", stderr);
        return;
    }

    VkPipelineStageFlags wait_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &f.img_avail;
    si.pWaitDstStageMask    = &wait_stages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &f.cb;
    si.signalSemaphoreCount = 1;
    // Signal the render-finished semaphore for THIS image (present waits on it).
    si.pSignalSemaphores    = &sc_render_done_[image_index_];
    VkResult sr = vkQueueSubmit(gfx_queue_, 1, &si, f.in_flight);
    if (sr == VK_ERROR_DEVICE_LOST) {
        std::fputs("[psy::gpu::vk] vkQueueSubmit: VK_ERROR_DEVICE_LOST — halting render\n", stderr);
        device_lost_ = true;
    } else if (sr != VK_SUCCESS) {
        std::fprintf(stderr, "[psy::gpu::vk] vkQueueSubmit failed: %s\n", result_str(sr));
    } else {
        // Record which frame this slot's fence now gates so begin_frame can
        // advance the GPU-completed watermark when it waits on the fence.
        slot_frame_[frame_slot_] = dev->current_frame_index;
    }
    w->open      = false;
    w->submitted = true;
}

void VulkanBackend::end_frame(Device* dev) {
    if (!swapchain_ready_ || device_lost_) return;

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &sc_render_done_[image_index_];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &swapchain_;
    pi.pImageIndices      = &image_index_;

    VkResult r = vkQueuePresentKHR(gfx_queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        resize_swapchain(dev, dev->swapchain_width, dev->swapchain_height);
    } else if (r == VK_ERROR_DEVICE_LOST) {
        std::fputs("[psy::gpu::vk] vkQueuePresentKHR: VK_ERROR_DEVICE_LOST — halting render\n", stderr);
        device_lost_ = true;
    } else if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[psy::gpu::vk] vkQueuePresentKHR failed: %s\n", result_str(r));
    }
    frame_slot_ = (frame_slot_ + 1) % kFramesInFlight;
}

void VulkanBackend::resize_swapchain(Device* dev, std::uint32_t w, std::uint32_t h) {
    if (surface_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE || device_lost_) return;

    // The surface's currentExtent is authoritative on Win32 / Wayland; fall
    // back to the requested size only when the surface defers to the app.
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) != VK_SUCCESS) return;
    const VkExtent2D want = (caps.currentExtent.width != 0xFFFFFFFFu)
        ? caps.currentExtent
        : VkExtent2D{ w, h };

    // Minimized (0-area): a 0-extent swapchain is invalid, so tear it down and
    // pause rendering until a non-zero size returns.
    if (want.width == 0 || want.height == 0) {
        if (swapchain_ready_) {
            vkDeviceWaitIdle(device_);
            destroy_swapchain_();
            swapchain_ready_ = false;
        }
        return;
    }

    // No actual change while healthy → don't rebuild. The game loop calls this
    // every frame; recreating the swapchain 60x/s would be a perf disaster and
    // churn GPU memory. (Only resize on a real size change or after
    // OUT_OF_DATE/SUBOPTIMAL forced us here.)
    if (swapchain_ready_ && want.width == sc_extent_.width && want.height == sc_extent_.height) {
        return;
    }

    vkDeviceWaitIdle(device_);
    // Device is idle here — flush any pending deferred-destroys unconditionally
    // rather than waiting for a future begin_frame to advance the watermark.
    reclaim_retired_(UINT64_MAX);
    // Per-frame objects (pools / cb / img_avail / fences) are size-independent
    // and survive a resize or a minimize; only the swapchain + its per-image
    // semaphores are rebuilt here. This also restores rendering after a
    // minimize tore the swapchain down.
    if (create_swapchain_(dev, want.width, want.height)) {
        swapchain_ready_ = true;
    }
}

// ─── Resource creation ──────────────────────────────────────────────────
//
// Samplers remain a minimal M0-stub; buffers now allocate a real VkBuffer +
// VkDeviceMemory (see create_buffer below — defined after find_memory_type).
// create_texture now wires the M1 upload path: when TextureDesc::initial_data
// is non-null we allocate a real VkImage, a host-visible staging VkBuffer,
// memcpy the source bytes in, and copy them onto the image with the
// canonical UNDEFINED → TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
// transition pair. Submission is synchronous: we wait on a one-shot fence
// before returning so the texture is ready by the time the Handle reaches
// the caller. The staging buffer is destroyed immediately after the wait.

// create_buffer() + ~VkBufferRes() are defined below, after the
// find_memory_type() helper they depend on.
Sampler* VulkanBackend::create_sampler(Device*) {
    auto* s = new (std::nothrow) VkSamplerRes();
    if (!s) return nullptr;
    if (device_ == VK_NULL_HANDLE) return s; // headless: null handle, no crash
    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod       = VK_LOD_CLAMP_NONE;
    sci.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (vkCreateSampler(device_, &sci, nullptr, &s->handle) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_sampler: vkCreateSampler failed\n", stderr);
        delete s;
        return nullptr;
    }
    s->device_for_destroy = device_;
    return s;
}
void*    VulkanBackend::buffer_map  (Buffer* b) { return static_cast<VkBufferRes*>(b)->mapped; }
void     VulkanBackend::buffer_unmap(Buffer*) {}

namespace {

// ─── Format → VkFormat + bytes_per_pixel mapping ────────────────────────
//
// M1 upload path supports an explicit allow-list. SRGB variants share
// byte layout with their Unorm peers but the gamma-channel decision
// belongs to the asset cooker (lane 09); deferred to M3.
inline std::uint32_t vk_bytes_per_pixel_for_upload(Format f) {
    switch (f) {
        case Format::Rgba8Unorm: return 4u;
        case Format::Rgba8Srgb:  return 4u;
        case Format::Bgra8Unorm: return 4u;
        case Format::Bgra8Srgb:  return 4u;
        case Format::R8Unorm:    return 1u;
        default:                 return 0u;
    }
}

inline VkFormat to_vk_format_for_upload(Format f) {
    switch (f) {
        case Format::Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        // sRGB sampled images: the hardware decodes sRGB→linear on read, so
        // display-authored texels (e.g. the sample's checkerboard) light the
        // shader in linear space and round-trip correctly to an sRGB swapchain.
        case Format::Rgba8Srgb:  return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::Bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::Bgra8Srgb:  return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::R8Unorm:    return VK_FORMAT_R8_UNORM;
        default:                 return VK_FORMAT_UNDEFINED;
    }
}

// Look up a memory type whose properties include every flag in `want`.
// Returns UINT32_MAX when no matching type exists in the type_bits mask.
inline std::uint32_t find_memory_type(VkPhysicalDevice phys,
                                      std::uint32_t type_bits,
                                      VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) == 0) continue;
        if ((mem.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

} // anonymous

// Real VkBuffer + VkDeviceMemory backing (replaces the M0 stub). BufferDesc
// carries no initial_data, so HostVisible buffers are filled by the caller via
// buffer_map() (the M1 vertex/index path); other heaps get device-local memory.
// (~VkBufferRes() is defined further down alongside ~VkTextureRes() in the
// psynder::gpu block — an out-of-class dtor must live in the class's namespace,
// not the inner vk_be one.)
Buffer* VulkanBackend::create_buffer(Device*, const BufferDesc& d) {
    auto* buf = new (std::nothrow) VkBufferRes();
    if (!buf) return nullptr;
    // Headless / zero-size: keep the M0 contract (null handle, no crash; a
    // draw that binds it simply no-ops).
    if (d.size_bytes == 0 || device_ == VK_NULL_HANDLE) return buf;

    VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (has_usage(d.usage, BufferUsage::Vertex))   usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (has_usage(d.usage, BufferUsage::Index))    usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (has_usage(d.usage, BufferUsage::Uniform))  usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (has_usage(d.usage, BufferUsage::Storage))  usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (has_usage(d.usage, BufferUsage::Indirect)) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = d.size_bytes;
    bci.usage       = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bci, nullptr, &buf->handle) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_buffer: vkCreateBuffer failed\n", stderr);
        delete buf;
        return nullptr;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buf->handle, &req);

    // BufferDesc has no initial_data, so every buffer is filled by the caller
    // via buffer_map() — pick a HOST_VISIBLE type so the map succeeds. For a
    // DeviceLocal heap, prefer DEVICE_LOCAL|HOST_VISIBLE (ReBAR / unified
    // memory — e.g. the RTX 5090 exposes this; fast for CPU write + GPU read),
    // falling back to plain host-visible. (A device-local-only buffer with a
    // staging upload is the M2+ path, once buffers carry initial_data.)
    constexpr VkMemoryPropertyFlags kHostVis =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::uint32_t mt = UINT32_MAX;
    if (d.heap != HeapKind::HostVisible) {
        mt = find_memory_type(phys_, req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | kHostVis);
    }
    if (mt == UINT32_MAX) {
        mt = find_memory_type(phys_, req.memoryTypeBits, kHostVis);
    }
    if (mt == UINT32_MAX) {
        std::fputs("[psy::gpu::vk] create_buffer: no host-visible memory type\n", stderr);
        vkDestroyBuffer(device_, buf->handle, nullptr);
        buf->handle = VK_NULL_HANDLE;
        delete buf;
        return nullptr;
    }

    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(device_, &mai, nullptr, &buf->mem) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_buffer: vkAllocateMemory failed\n", stderr);
        vkDestroyBuffer(device_, buf->handle, nullptr);
        buf->handle = VK_NULL_HANDLE;
        delete buf;
        return nullptr;
    }
    vkBindBufferMemory(device_, buf->handle, buf->mem, 0);

    // Persistently map (every buffer is host-visible + coherent — no flush
    // needed) so buffer_map() returns the pointer at zero per-call cost; the
    // dtor unmaps.
    if (vkMapMemory(device_, buf->mem, 0, VK_WHOLE_SIZE, 0, &buf->mapped) != VK_SUCCESS) {
        buf->mapped = nullptr;
    }
    buf->device_for_destroy = device_;
    return buf;
}

Texture* VulkanBackend::create_texture(Device* dev, const TextureDesc& desc) {
    auto* tex = new (std::nothrow) VkTextureRes();
    if (!tex) return nullptr;

    // Fast path: no initial_data → preserve the M0 stub behaviour
    // (handle / view / mem remain VK_NULL_HANDLE).
    if (desc.initial_data == nullptr) {
        return tex;
    }

    // Validate the format + dimensions + buffer size before we touch any
    // Vulkan API. Returning nullptr here is the "no crash, invalid handle"
    // contract from PublicGpu.h.
    const std::uint32_t bpp = vk_bytes_per_pixel_for_upload(desc.format);
    const VkFormat vk_fmt   = to_vk_format_for_upload(desc.format);
    if (bpp == 0 || vk_fmt == VK_FORMAT_UNDEFINED) {
        std::fputs("[psy::gpu::vk] create_texture: format has no initial_data path yet (M1 supports Rgba8Unorm/Rgba8Srgb/Bgra8Unorm/Bgra8Srgb/R8Unorm)\n", stderr);
        delete tex;
        return nullptr;
    }
    if (desc.width == 0 || desc.height == 0) {
        std::fputs("[psy::gpu::vk] create_texture: initial_data given but width/height is 0\n", stderr);
        delete tex;
        return nullptr;
    }
    const std::size_t expected = static_cast<std::size_t>(desc.width)
                               * static_cast<std::size_t>(desc.height)
                               * static_cast<std::size_t>(bpp);
    if (desc.initial_data_size != expected) {
        std::fprintf(stderr,
            "[psy::gpu::vk] create_texture: initial_data_size=%zu != expected %zu (%ux%u, %u bpp)\n",
            desc.initial_data_size, expected, desc.width, desc.height, bpp);
        delete tex;
        return nullptr;
    }
    if (device_ == VK_NULL_HANDLE || phys_ == VK_NULL_HANDLE) {
        std::fputs("[psy::gpu::vk] create_texture: backend not initialised (no VkDevice)\n", stderr);
        delete tex;
        return nullptr;
    }

    // ─── Image + image memory ────────────────────────────────────────────
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = vk_fmt;
    ici.extent        = {desc.width, desc.height, 1u};
    ici.mipLevels     = 1u;
    ici.arrayLayers   = 1u;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has_usage(desc.usage, TextureUsage::Storage))     ici.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (has_usage(desc.usage, TextureUsage::RenderTarget))ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has_usage(desc.usage, TextureUsage::TransferSrc)) ici.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device_, &ici, nullptr, &image) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkCreateImage failed\n", stderr);
        delete tex;
        return nullptr;
    }

    VkMemoryRequirements img_req{};
    vkGetImageMemoryRequirements(device_, image, &img_req);
    const std::uint32_t img_mt = find_memory_type(phys_, img_req.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_mt == UINT32_MAX) {
        std::fputs("[psy::gpu::vk] create_texture: no DEVICE_LOCAL memory type for image\n", stderr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
        return nullptr;
    }
    VkMemoryAllocateInfo img_mai{};
    img_mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    img_mai.allocationSize  = img_req.size;
    img_mai.memoryTypeIndex = img_mt;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &img_mai, nullptr, &img_mem) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkAllocateMemory(image) failed\n", stderr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
        return nullptr;
    }
    if (vkBindImageMemory(device_, image, img_mem, 0) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkBindImageMemory failed\n", stderr);
        vkFreeMemory(device_, img_mem, nullptr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
        return nullptr;
    }

    // ─── Staging buffer (host-visible) ───────────────────────────────────
    VkBufferCreateInfo sbci{};
    sbci.sType        = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    sbci.size         = static_cast<VkDeviceSize>(expected);
    sbci.usage        = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    sbci.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer staging_buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(device_, &sbci, nullptr, &staging_buf) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkCreateBuffer(staging) failed\n", stderr);
        vkFreeMemory(device_, img_mem, nullptr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
        return nullptr;
    }
    VkMemoryRequirements sb_req{};
    vkGetBufferMemoryRequirements(device_, staging_buf, &sb_req);
    const std::uint32_t sb_mt = find_memory_type(phys_, sb_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (sb_mt == UINT32_MAX) {
        std::fputs("[psy::gpu::vk] create_texture: no HOST_VISIBLE|HOST_COHERENT memory type for staging\n", stderr);
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, img_mem, nullptr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
        return nullptr;
    }
    VkMemoryAllocateInfo sb_mai{};
    sb_mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    sb_mai.allocationSize  = sb_req.size;
    sb_mai.memoryTypeIndex = sb_mt;
    VkDeviceMemory sb_mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &sb_mai, nullptr, &sb_mem) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkAllocateMemory(staging) failed\n", stderr);
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, img_mem, nullptr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
        return nullptr;
    }
    if (vkBindBufferMemory(device_, staging_buf, sb_mem, 0) != VK_SUCCESS) {
        // Without a bound buffer, vkMapMemory below would map fresh
        // allocator-owned bytes that the buffer can't reach — the GPU
        // copy would then read uninitialised contents.  Clean up and
        // bail out.  Copilot PR #15 review caught the missing check.
        std::fputs("[psy::gpu::vk] create_texture: vkBindBufferMemory(staging) failed\n", stderr);
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, sb_mem, nullptr);
        vkFreeMemory(device_, img_mem, nullptr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
        return nullptr;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device_, sb_mem, 0, sb_req.size, 0, &mapped) != VK_SUCCESS || !mapped) {
        std::fputs("[psy::gpu::vk] create_texture: vkMapMemory(staging) failed\n", stderr);
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, sb_mem, nullptr);
        vkFreeMemory(device_, img_mem, nullptr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
        return nullptr;
    }
    std::memcpy(mapped, desc.initial_data, expected);
    vkUnmapMemory(device_, sb_mem);

    // ─── One-shot transfer command buffer ────────────────────────────────
    VkCommandPool   xfer_pool = VK_NULL_HANDLE;
    VkCommandBuffer xfer_cb   = VK_NULL_HANDLE;
    VkFence         xfer_fence = VK_NULL_HANDLE;

    auto fail_cleanup = [&]() {
        if (xfer_fence) vkDestroyFence(device_, xfer_fence, nullptr);
        if (xfer_pool)  vkDestroyCommandPool(device_, xfer_pool, nullptr);
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, sb_mem, nullptr);
        vkFreeMemory(device_, img_mem, nullptr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
    };

    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = gfx_queue_idx_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &xfer_pool) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkCreateCommandPool(upload) failed\n", stderr);
        fail_cleanup();
        return nullptr;
    }
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = xfer_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &cbai, &xfer_cb) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkAllocateCommandBuffers(upload) failed\n", stderr);
        fail_cleanup();
        return nullptr;
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(xfer_cb, &bi);

    // UNDEFINED → TRANSFER_DST_OPTIMAL
    {
        VkImageMemoryBarrier b{};
        b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask    = 0;
        b.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.image            = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(xfer_cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = 0; // tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset       = {0, 0, 0};
    region.imageExtent       = {desc.width, desc.height, 1u};
    vkCmdCopyBufferToImage(xfer_cb, staging_buf, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.  Widen dstStage
    // to include all shader stages (vertex / fragment / compute) so
    // a texture used by any subsequent pass sees the transfer writes.
    // Earlier this barrier used only FRAGMENT_SHADER_BIT, which would
    // have silently produced a hazard for compute / vertex consumers
    // (Copilot PR #15 review).  ALL_GRAPHICS covers VS/TCS/TES/GS/FS
    // in one bit; add COMPUTE_SHADER_BIT for explicit clarity.
    {
        VkImageMemoryBarrier b{};
        b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        b.image            = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(xfer_cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    vkEndCommandBuffer(xfer_cb);

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device_, &fci, nullptr, &xfer_fence) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkCreateFence(upload) failed\n", stderr);
        fail_cleanup();
        return nullptr;
    }

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &xfer_cb;
    if (vkQueueSubmit(gfx_queue_, 1, &si, xfer_fence) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkQueueSubmit(upload) failed\n", stderr);
        fail_cleanup();
        return nullptr;
    }
    // Synchronous wait — M3 will batch via the JobSystem; for M1 we block.
    vkWaitForFences(device_, 1, &xfer_fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(device_, xfer_fence, nullptr);
    vkDestroyCommandPool(device_, xfer_pool, nullptr); // implicitly frees xfer_cb
    vkDestroyBuffer(device_, staging_buf, nullptr);
    vkFreeMemory(device_, sb_mem, nullptr);

    // ─── Sampled image view for shader binding ───────────────────────────
    VkImageViewCreateInfo vci{};
    vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image            = image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = vk_fmt;
    vci.components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device_, &vci, nullptr, &view) != VK_SUCCESS) {
        std::fputs("[psy::gpu::vk] create_texture: vkCreateImageView failed\n", stderr);
        vkFreeMemory(device_, img_mem, nullptr);
        vkDestroyImage(device_, image, nullptr);
        delete tex;
        return nullptr;
    }

    tex->handle             = image;
    tex->view               = view;
    tex->mem                = img_mem;
    tex->device_for_destroy = device_;
    (void)dev;
    return tex;
}

void VulkanBackend::destroy_resource(RefCountedBase* res) {
    // Defer the delete: the GPU may still reference `res` from an in-flight
    // command buffer (e.g. a buffer/texture/sampler bound in the last 1-2
    // submitted frames). Tag with the owning device's current frame index —
    // conservative but correct, since a resource whose handle is gone can't be
    // bound by any *future* frame — and reclaim once that frame has retired.
    // The virtual ~VkTextureRes()/~VkBufferRes()/~VkSamplerRes() releases the
    // Vulkan handles when we eventually delete (no RTTI / downcast needed —
    // see VulkanBackend.h for the rationale).
    if (!res) return;
    Device* dev = res->owner();
    const std::uint64_t frame = dev ? dev->current_frame_index : 0;
    std::lock_guard<std::mutex> lock(pending_destroy_mu_);
    pending_destroy_.push_back({res, frame});
}

void VulkanBackend::reclaim_retired_(std::uint64_t through_frame) {
    std::lock_guard<std::mutex> lock(pending_destroy_mu_);
    std::size_t w = 0;
    for (std::size_t r = 0; r < pending_destroy_.size(); ++r) {
        if (pending_destroy_[r].frame <= through_frame) {
            delete pending_destroy_[r].res; // virtual dtor frees Vulkan objects
        } else {
            pending_destroy_[w++] = pending_destroy_[r];
        }
    }
    pending_destroy_.resize(w);
}

// ─── Test-only: mip-0 readback ─────────────────────────────────────────
//
// Synchronous copy of mip-0 back to a host buffer. NOT a public ABI —
// invoked only by tests/unit/gpu_texture_upload.cpp through the
// Backend::texture_readback_mip0 virtual. Implementation:
//   * Allocate a host-visible staging buffer.
//   * Transition the image SHADER_READ_ONLY → TRANSFER_SRC_OPTIMAL.
//   * vkCmdCopyImageToBuffer; transition back to SHADER_READ_ONLY.
//   * Submit, wait, memcpy out, destroy the staging buffer.
bool VulkanBackend::texture_readback_mip0(Texture* base_tex,
                                          void* out_dst_bytes,
                                          std::size_t dst_bytes_size) {
    if (!base_tex || !out_dst_bytes || dst_bytes_size == 0) return false;
    auto* tex = static_cast<VkTextureRes*>(base_tex);
    if (tex->handle == VK_NULL_HANDLE) return false;
    if (device_ == VK_NULL_HANDLE || phys_ == VK_NULL_HANDLE) return false;
    if (gfx_queue_ == VK_NULL_HANDLE) return false;

    const TextureDesc& desc = tex->desc;
    const std::uint32_t bpp = vk_bytes_per_pixel_for_upload(desc.format);
    if (bpp == 0) return false;
    const std::size_t expected = static_cast<std::size_t>(desc.width)
                               * static_cast<std::size_t>(desc.height)
                               * static_cast<std::size_t>(bpp);
    if (expected == 0 || dst_bytes_size != expected) return false;

    // Host-visible staging buffer.
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci{};
        bci.sType        = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size         = static_cast<VkDeviceSize>(expected);
        bci.usage        = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bci, nullptr, &staging_buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, staging_buf, &req);
        const std::uint32_t mt = find_memory_type(phys_, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX) {
            vkDestroyBuffer(device_, staging_buf, nullptr);
            return false;
        }
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = mt;
        if (vkAllocateMemory(device_, &mai, nullptr, &staging_mem) != VK_SUCCESS) {
            vkDestroyBuffer(device_, staging_buf, nullptr);
            return false;
        }
        vkBindBufferMemory(device_, staging_buf, staging_mem, 0);
    }

    // One-shot command buffer + fence.
    VkCommandPool   pool  = VK_NULL_HANDLE;
    VkCommandBuffer cb    = VK_NULL_HANDLE;
    VkFence         fence = VK_NULL_HANDLE;
    auto teardown = [&](){
        if (fence) vkDestroyFence(device_, fence, nullptr);
        if (pool)  vkDestroyCommandPool(device_, pool, nullptr);
        if (staging_buf) vkDestroyBuffer(device_, staging_buf, nullptr);
        if (staging_mem) vkFreeMemory(device_, staging_mem, nullptr);
    };

    {
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = gfx_queue_idx_;
        if (vkCreateCommandPool(device_, &pci, nullptr, &pool) != VK_SUCCESS) {
            teardown();
            return false;
        }
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &cbai, &cb) != VK_SUCCESS) {
            teardown();
            return false;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &fci, nullptr, &fence) != VK_SUCCESS) {
            // Without a valid fence we can't safely vkWaitForFences after
            // submit — fall back to teardown rather than feeding
            // VK_NULL_HANDLE into wait (validation error / crash).
            // Copilot PR #15 review caught the missing check.
            teardown();
            return false;
        }
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // SHADER_READ_ONLY → TRANSFER_SRC_OPTIMAL — match the producer-side
    // barrier's stage breadth (all shader stages) since the prior
    // upload path may have left the texture readable from compute /
    // vertex / fragment.  Use ALL_GRAPHICS to cover all of them in one
    // mask plus VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT for compute pass
    // consumers.  Copilot PR #15 review caught the narrow mask.
    {
        VkImageMemoryBarrier b{};
        b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
        b.image            = tex->handle;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    VkBufferImageCopy r{};
    r.bufferOffset      = 0;
    r.bufferRowLength   = 0;
    r.bufferImageHeight = 0;
    r.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageOffset       = {0, 0, 0};
    r.imageExtent       = {desc.width, desc.height, 1u};
    vkCmdCopyImageToBuffer(cb, tex->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging_buf, 1, &r);

    // TRANSFER_SRC_OPTIMAL → SHADER_READ_ONLY_OPTIMAL (restore).  Same
    // widened dstStage as the upload path — see comment above.
    {
        VkImageMemoryBarrier b{};
        b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        b.image            = tex->handle;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    vkEndCommandBuffer(cb);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    if (vkQueueSubmit(gfx_queue_, 1, &si, fence) != VK_SUCCESS) {
        teardown();
        return false;
    }
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);

    void* mapped = nullptr;
    bool ok = (vkMapMemory(device_, staging_mem, 0, expected, 0, &mapped) == VK_SUCCESS && mapped);
    if (ok) {
        std::memcpy(out_dst_bytes, mapped, expected);
        vkUnmapMemory(device_, staging_mem);
    }

    teardown();
    return ok;
}

// Texture destructor — release the owned VkImage / VkImageView /
// VkDeviceMemory in reverse creation order. Safe when called on an
// M0-stub texture (all handles null and `device_for_destroy` null).
// Defined out-of-line so the body can call Vulkan loader entry points
// without forcing the header to drag <volk.h> into every consumer.
//
// Fully-qualified `::psynder::gpu::VkTextureRes` because this definition
// sits inside the `psynder::gpu::vk_be` namespace block (the rest of the
// VulkanBackend impl). The unqualified form `VkTextureRes::~VkTextureRes`
// would attempt to bind to `psynder::gpu::vk_be::VkTextureRes` which
// doesn't exist.
} // namespace vk_be
} // namespace psynder::gpu

namespace psynder::gpu {
VkTextureRes::~VkTextureRes() {
    if (device_for_destroy != VK_NULL_HANDLE) {
        if (view)   vkDestroyImageView(device_for_destroy, view, nullptr);
        if (handle) vkDestroyImage    (device_for_destroy, handle, nullptr);
        if (mem)    vkFreeMemory      (device_for_destroy, mem, nullptr);
    }
    view   = VK_NULL_HANDLE;
    handle = VK_NULL_HANDLE;
    mem    = VK_NULL_HANDLE;
}

VkBufferRes::~VkBufferRes() {
    if (device_for_destroy != VK_NULL_HANDLE) {
        if (mapped) vkUnmapMemory  (device_for_destroy, mem);
        if (handle) vkDestroyBuffer(device_for_destroy, handle, nullptr);
        if (mem)    vkFreeMemory   (device_for_destroy, mem, nullptr);
    }
    mapped = nullptr;
    handle = VK_NULL_HANDLE;
    mem    = VK_NULL_HANDLE;
}

VkSamplerRes::~VkSamplerRes() {
    if (device_for_destroy != VK_NULL_HANDLE && handle != VK_NULL_HANDLE)
        vkDestroySampler(device_for_destroy, handle, nullptr);
    handle = VK_NULL_HANDLE;
}
} // namespace psynder::gpu

namespace psynder::gpu {
namespace vk_be {

// ─── Render-encoder API (lane09-001 unblock) ────────────────────────────
//
// Cross-lane handshake: lane 08's PipelineHandle is a uint32_t id; the
// VkPipeline + VkPipelineLayout objects live in lane 08's registry (once
// it ships PSO creation). Until then, lane 07 keeps a small shim
// registry that lane 08 populates via psynder_gx_vk_register_pipeline().
// bind_pipeline against an un-registered handle logs once and renders
// nothing (no crash) — matching the Metal behavior.

namespace vk_pipeline_shim {

constexpr std::uint32_t kMaxPipelines = 64;

struct Entry {
    std::uint32_t         id          = 0;
    VkPipeline            pipeline    = VK_NULL_HANDLE;
    VkPipelineLayout      layout      = VK_NULL_HANDLE;
    VkPipelineBindPoint   bind_point  = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

// Writers: psynder_gx_vk_register_pipeline() called from lane 08's worker
// threads after a VkPipeline is built.  Reader: bind_pipeline() runs on
// the render thread.  Both take g_mu.  The shim is small (<= 64 entries)
// and lookups are linear, so a plain std::mutex is fine — readers and
// writers don't contend much in practice (registration is bursty at
// startup / hot-reload, binds are per-frame).
static Entry        g_entries[kMaxPipelines] = {};
static std::uint32_t g_count = 0;
static std::mutex   g_mu;

// Locked find — caller must already hold g_mu.
static Entry* find_locked(std::uint32_t id) {
    for (std::uint32_t i = 0; i < g_count; ++i) {
        if (g_entries[i].id == id) return &g_entries[i];
    }
    return nullptr;
}

} // namespace vk_pipeline_shim

void VulkanBackend::destroy_registered_pipelines_() {
    if (device_ == VK_NULL_HANDLE) return;
    namespace s = vk_pipeline_shim;
    std::lock_guard<std::mutex> _(s::g_mu);
    for (std::uint32_t i = 0; i < s::g_count; ++i) {
        if (s::g_entries[i].pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device_, s::g_entries[i].pipeline, nullptr);
        if (s::g_entries[i].layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, s::g_entries[i].layout, nullptr);
        s::g_entries[i] = {};
    }
    s::g_count = 0;
}

} // namespace vk_be

// Public-but-lane-internal hooks lane 08 will call once it ships Vulkan
// pipeline-state emission. `extern "C"` so lane 08 can forward-declare
// the prototypes without including Vulkan headers.
extern "C" {

void psynder_gx_vk_register_pipeline(std::uint32_t id,
                                     void*         vk_pipeline,
                                     void*         vk_pipeline_layout,
                                     std::uint32_t bind_point /*0=gfx 1=compute*/) {
    namespace s = psynder::gpu::vk_be::vk_pipeline_shim;
    VkPipelineBindPoint bp = (bind_point == 1)
        ? VK_PIPELINE_BIND_POINT_COMPUTE
        : VK_PIPELINE_BIND_POINT_GRAPHICS;
    // VkPipeline / VkPipelineLayout are non-dispatchable handles. On 64-bit
    // platforms (the supported targets — Win64, Linux x86_64) they are
    // pointer-sized; reinterpret_cast<VkPipeline>(void*) compiles cleanly
    // under VK_DEFINE_NON_DISPATCHABLE_HANDLE's default pointer form.
    VkPipeline       pso    = reinterpret_cast<VkPipeline>(vk_pipeline);
    VkPipelineLayout layout = reinterpret_cast<VkPipelineLayout>(vk_pipeline_layout);

    // Lock the shim — lane 08 may call this from a worker thread while the
    // render thread is in bind_pipeline() walking g_entries.
    std::lock_guard<std::mutex> _(s::g_mu);
    auto& cnt = s::g_count;
    if (auto* e = s::find_locked(id)) {
        e->pipeline   = pso;
        e->layout     = layout;
        e->bind_point = bp;
        return;
    }
    if (cnt >= s::kMaxPipelines) {
        std::fputs("[psy::gpu::vk] pipeline_shim: too many registrations\n", stderr);
        return;
    }
    s::g_entries[cnt].id         = id;
    s::g_entries[cnt].pipeline   = pso;
    s::g_entries[cnt].layout     = layout;
    s::g_entries[cnt].bind_point = bp;
    ++cnt;
}

} // extern "C"

namespace vk_be {

namespace {

VkAttachmentLoadOp to_vk_load(LoadOp op) {
    switch (op) {
        case LoadOp::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear:    return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

VkAttachmentStoreOp to_vk_store(StoreOp op) {
    switch (op) {
        case StoreOp::Store:    return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreOp::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_STORE_OP_STORE;
}

} // anonymous

void VulkanBackend::begin_render(CmdBuffer* cmd, const RenderPassDesc& desc) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb) return;

    // Map the user's color attachments to VkRenderingAttachmentInfo. For
    // the M0 swapchain path we also reuse the existing barrier helpers so
    // the image lands in COLOR_ATTACHMENT_OPTIMAL before the dynamic-render
    // begin call.
    const std::uint32_t color_count = desc.color_count > 8 ? 8u : desc.color_count;
    VkRenderingAttachmentInfo color_infos[8] = {};
    bool                       to_swapchain_image = false;
    VkImage                    sc_image = VK_NULL_HANDLE;
    VkExtent2D                 extent = sc_extent_;

    for (std::uint32_t i = 0; i < color_count; ++i) {
        const ColorAttachment& a = desc.colors[i];

        VkImageView view = VK_NULL_HANDLE;
        if (desc.swapchain || a.target == nullptr) {
            // First color = swapchain; remaining swapchain attachments
            // unsupported (driver expects matching extents anyway).
            if (i == 0 && swapchain_ready_) {
                view              = sc_views_[image_index_];
                sc_image          = sc_images_[image_index_];
                to_swapchain_image = true;
            }
        } else {
            view = static_cast<VkTextureRes*>(a.target)->view;
        }
        if (view == VK_NULL_HANDLE) continue;

        color_infos[i].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_infos[i].imageView   = view;
        color_infos[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_infos[i].loadOp      = to_vk_load (a.load);
        color_infos[i].storeOp     = to_vk_store(a.store);
        color_infos[i].clearValue.color = {{
            a.clear_rgba[0], a.clear_rgba[1],
            a.clear_rgba[2], a.clear_rgba[3]
        }};
    }

    VkRenderingAttachmentInfo depth_info = {};
    bool                      has_depth = false;
    if (desc.depth.target) {
        auto* dt = static_cast<VkTextureRes*>(desc.depth.target);
        if (dt->view != VK_NULL_HANDLE) {
            depth_info.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_info.imageView   = dt->view;
            depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_info.loadOp      = to_vk_load (desc.depth.load);
            depth_info.storeOp     = to_vk_store(desc.depth.store);
            depth_info.clearValue.depthStencil.depth   = desc.depth.clear_depth;
            depth_info.clearValue.depthStencil.stencil = desc.depth.clear_stencil;
            has_depth = true;
        }
    }

    // Swapchain image needs layout transition before we render into it.
    if (to_swapchain_image && sc_image != VK_NULL_HANDLE) {
        barrier_to_color_attachment(w->cb, sc_image);
    }

    VkRenderingInfo ri{};
    ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset    = {0, 0};
    ri.renderArea.extent    = extent;
    ri.layerCount           = 1;
    ri.colorAttachmentCount = color_count;
    ri.pColorAttachments    = color_count > 0 ? color_infos : nullptr;
    ri.pDepthAttachment     = has_depth ? &depth_info : nullptr;

    vkCmdBeginRendering(w->cb, &ri);

    w->in_render_pass            = true;
    w->render_pass_to_swapchain  = to_swapchain_image;
    w->swapchain_image_for_pass  = sc_image;
    w->encoded_clear             = true; // suppress the M0 auto-clear path
}

void VulkanBackend::end_render(CmdBuffer* cmd) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb || !w->in_render_pass) return;
    vkCmdEndRendering(w->cb);
    if (w->render_pass_to_swapchain && w->swapchain_image_for_pass != VK_NULL_HANDLE) {
        barrier_to_present(w->cb, w->swapchain_image_for_pass);
    }
    w->in_render_pass            = false;
    w->render_pass_to_swapchain  = false;
    w->swapchain_image_for_pass  = VK_NULL_HANDLE;
}

void VulkanBackend::set_viewport(CmdBuffer* cmd, const Viewport& vp) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb) return;
    VkViewport v{};
    v.x        = vp.x;
    v.y        = vp.y;
    v.width    = vp.w;
    v.height   = vp.h;
    v.minDepth = vp.min_depth;
    v.maxDepth = vp.max_depth;
    vkCmdSetViewport(w->cb, 0, 1, &v);
}

void VulkanBackend::set_scissor(CmdBuffer* cmd, const Scissor& sc) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb) return;
    VkRect2D r{};
    r.offset.x = sc.x;
    r.offset.y = sc.y;
    r.extent.width  = sc.w;
    r.extent.height = sc.h;
    vkCmdSetScissor(w->cb, 0, 1, &r);
}

void VulkanBackend::bind_pipeline(CmdBuffer* cmd, ::psynder::shader::PipelineHandle h) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb) return;
    if (!h.valid()) {
        w->bound_pipeline        = VK_NULL_HANDLE;
        w->bound_pipeline_layout = VK_NULL_HANDLE;
        return;
    }
    // Snapshot the shim under the lock so a concurrent
    // psynder_gx_vk_register_pipeline() from a worker thread doesn't tear
    // the read.  Copy the small Entry POD out then release the lock —
    // vkCmdBindPipeline() doesn't need to be serialised against the shim.
    VkPipeline           pipeline    = VK_NULL_HANDLE;
    VkPipelineLayout     layout      = VK_NULL_HANDLE;
    VkPipelineBindPoint  bind_point  = VK_PIPELINE_BIND_POINT_GRAPHICS;
    bool                 found       = false;
    {
        std::lock_guard<std::mutex> _(vk_pipeline_shim::g_mu);
        if (auto* e = vk_pipeline_shim::find_locked(h.id);
            e && e->pipeline != VK_NULL_HANDLE) {
            pipeline   = e->pipeline;
            layout     = e->layout;
            bind_point = e->bind_point;
            found      = true;
        }
    }
    if (!found) {
        // Lane 08 hasn't published a VkPipeline for this handle yet.
        static std::uint32_t s_last_warned_id = 0;
        if (h.id != s_last_warned_id) {
            std::fprintf(stderr,
                "[psy::gpu::vk] bind_pipeline: PipelineHandle id=%u not registered "
                "(lane 08 owes psynder_gx_vk_register_pipeline); draws will no-op\n",
                h.id);
            s_last_warned_id = h.id;
        }
        w->bound_pipeline        = VK_NULL_HANDLE;
        w->bound_pipeline_layout = VK_NULL_HANDLE;
        return;
    }
    w->bound_pipeline        = pipeline;
    w->bound_pipeline_layout = layout;
    w->bound_bind_point      = bind_point;
    vkCmdBindPipeline(w->cb, bind_point, pipeline);
}

void VulkanBackend::bind_vertex_buffer(CmdBuffer* cmd, std::uint32_t binding,
                                       Buffer* buf, std::uint64_t offset) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb || !buf) return;
    VkBuffer h = static_cast<VkBufferRes*>(buf)->handle;
    if (h == VK_NULL_HANDLE) return;
    VkDeviceSize off = offset;
    vkCmdBindVertexBuffers(w->cb, binding, 1, &h, &off);
}

void VulkanBackend::bind_index_buffer(CmdBuffer* cmd, Buffer* buf,
                                      IndexType type, std::uint64_t offset) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb || !buf) return;
    VkBuffer h = static_cast<VkBufferRes*>(buf)->handle;
    if (h == VK_NULL_HANDLE) return;
    VkIndexType it = (type == IndexType::U32) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    vkCmdBindIndexBuffer(w->cb, h, (VkDeviceSize)offset, it);
}

void VulkanBackend::bind_texture(CmdBuffer* cmd, std::uint32_t slot,
                                 Texture* tex, Sampler* samp) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb || !tex || !samp) return;
    if (w->bound_pipeline_layout == VK_NULL_HANDLE) {
        // No pipeline bound yet — the layout that the set binds against is
        // unknown. bind_pipeline already logs on an unknown handle; stay quiet.
        return;
    }
    VkImageView view = static_cast<VkTextureRes*>(tex)->view;
    VkSampler   smp  = static_cast<VkSamplerRes*>(samp)->handle;
    if (view == VK_NULL_HANDLE || smp == VK_NULL_HANDLE) return;

    // M1: the sampled-image + sampler pair is descriptor set `slot` (the
    // sample binds slot 0, matching the shader's [[vk::binding(_,0)]] set).
    VkDescriptorSet set = m1_tex_descriptor_set(device_, view, smp);
    if (set == VK_NULL_HANDLE) return;
    vkCmdBindDescriptorSets(w->cb, w->bound_bind_point, w->bound_pipeline_layout,
                            slot, 1, &set, 0, nullptr);
}

void VulkanBackend::push_constants(CmdBuffer* cmd, const void* data,
                                   std::uint32_t size, std::uint32_t stage_mask) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb || !data || size == 0) return;
    if (w->bound_pipeline_layout == VK_NULL_HANDLE) {
        // No layout bound — can't issue push constants. Drop the call;
        // a warning is already emitted by bind_pipeline when the handle
        // is unknown so we don't double-log here.
        return;
    }
    VkShaderStageFlags vk_stages = 0;
    if (stage_mask & 1u /*Vertex*/)   vk_stages |= VK_SHADER_STAGE_VERTEX_BIT;
    if (stage_mask & 2u /*Fragment*/) vk_stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (stage_mask & 4u /*Compute*/)  vk_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
    if (stage_mask & 8u /*Mesh*/)     vk_stages |= VK_SHADER_STAGE_MESH_BIT_EXT;
    if (vk_stages == 0) {
        // Caller passed an unknown mask — default to VERTEX|FRAGMENT, which is
        // what M1 graphics pipeline layouts declare their push range over.
        // (ALL_GRAPHICS would add tess/geometry stages absent from the range
        // and trip VUID-vkCmdPushConstants-offset-01795.)
        vk_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    vkCmdPushConstants(w->cb, w->bound_pipeline_layout, vk_stages, 0, size, data);
}

void VulkanBackend::draw(CmdBuffer* cmd, std::uint32_t vc, std::uint32_t ic,
                         std::uint32_t fv, std::uint32_t fi) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb) return;
    if (w->bound_pipeline == VK_NULL_HANDLE) return;
    vkCmdDraw(w->cb, vc, ic ? ic : 1u, fv, fi);
}

void VulkanBackend::draw_indexed(CmdBuffer* cmd, std::uint32_t ic, std::uint32_t inst,
                                 std::uint32_t fi, std::int32_t vo, std::uint32_t fii) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb) return;
    if (w->bound_pipeline == VK_NULL_HANDLE) return;
    vkCmdDrawIndexed(w->cb, ic, inst ? inst : 1u, fi, vo, fii);
}

void VulkanBackend::dispatch(CmdBuffer* cmd, std::uint32_t gx,
                             std::uint32_t gy, std::uint32_t gz) {
    auto* w = static_cast<VkCmdBuf*>(cmd);
    if (!w || !w->cb) return;
    if (w->in_render_pass) {
        std::fputs("[psy::gpu::vk] dispatch called inside a render pass; ignored\n", stderr);
        return;
    }
    if (w->bound_pipeline == VK_NULL_HANDLE ||
        w->bound_bind_point != VK_PIPELINE_BIND_POINT_COMPUTE) {
        return;
    }
    vkCmdDispatch(w->cb, gx ? gx : 1u, gy ? gy : 1u, gz ? gz : 1u);
    w->encoded_clear = true; // suppress auto-clear if the only work was compute
}

} // namespace vk_be

// ─── create_backend factory — Vulkan build ──────────────────────────────
Backend* create_backend() {
    return vk_be::make_vulkan_backend();
}

} // namespace psynder::gpu

#endif // PSYNDER_GX_BACKEND_VULKAN
