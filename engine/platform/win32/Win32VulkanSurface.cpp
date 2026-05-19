// SPDX-License-Identifier: MIT
// Psynder-GX — Win32 Vulkan surface creation (vkCreateWin32SurfaceKHR).
//
// The VK_KHR_win32_surface extension must be enabled when the VkInstance is
// created (lane 07 is responsible for requesting it via VkInstanceCreateInfo).
// We resolve PFN_vkCreateWin32SurfaceKHR at call time via
// vkGetInstanceProcAddr so this TU does NOT require vulkan-1.lib to be linked
// directly — it piggybacks on whatever Vulkan loader lane 07 links in.
//
// This file compiles on Windows only (PSYNDER_PLATFORM_WIN32). Non-Windows
// builds exclude this TU entirely via the lane CMakeLists platform guard.

#include "Win32VulkanSurface.h"

#if defined(PSYNDER_PLATFORM_WIN32)

#include "core/Log.h"
#include <cstdint>

namespace psynder::platform::win32 {

VkSurfaceKHR create_vulkan_surface(HWND hwnd, VkInstance vk_instance)
{
    if (!hwnd) {
        PSY_LOG_ERROR("[win32] create_vulkan_surface: hwnd is null");
        return VK_NULL_HANDLE;
    }
    if (!vk_instance) {
        PSY_LOG_ERROR("[win32] create_vulkan_surface: vk_instance is null");
        return VK_NULL_HANDLE;
    }

    // Resolve the surface-creation entry point from the loader at runtime.
    // Using vkGetInstanceProcAddr (rather than linking vkCreateWin32SurfaceKHR
    // directly) keeps this TU decoupled from the specific Vulkan loader DLL
    // that lane 07 loads. On a machine without the Vulkan SDK only the
    // vulkan-1.dll (runtime redist) is needed.
    //
    // NEEDS PC VALIDATION: the extension entry point lookup chain is:
    //   vkGetInstanceProcAddr → VK_KHR_win32_surface in the loaded ICD
    // If the ICD does not expose VK_KHR_win32_surface the proc will be null
    // and we return VK_NULL_HANDLE with a log message.
    //
    // Lane 07 must request VK_KHR_surface + VK_KHR_win32_surface in
    // VkInstanceCreateInfo::ppEnabledExtensionNames before calling this.
    auto pfn_create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        vkGetInstanceProcAddr(vk_instance, "vkCreateWin32SurfaceKHR"));

    if (!pfn_create) {
        PSY_LOG_ERROR("[win32] vkGetInstanceProcAddr(\"vkCreateWin32SurfaceKHR\") returned null. "
                      "Ensure VK_KHR_win32_surface is enabled in the VkInstance.");
        return VK_NULL_HANDLE;
    }

    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.pNext     = nullptr;
    ci.flags     = 0;  // reserved, must be 0
    ci.hinstance = ::GetModuleHandleW(nullptr);  // this process's HINSTANCE
    ci.hwnd      = hwnd;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result = pfn_create(vk_instance, &ci, nullptr, &surface);
    if (result != VK_SUCCESS) {
        PSY_LOG_ERROR("[win32] vkCreateWin32SurfaceKHR failed: VkResult={}",
                      static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    // VkSurfaceKHR is a VK_DEFINE_NON_DISPATCHABLE_HANDLE — on 64-bit it is a
    // pointer, on 32-bit it is a uint64_t. Cast through uint64_t to keep the
    // log portable on both ABIs.
    PSY_LOG_INFO("[win32] Vulkan surface created (VkSurfaceKHR=0x{:016x})",
                 static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(surface)));
    return surface;
}

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
