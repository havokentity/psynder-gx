// SPDX-License-Identifier: MIT
// Psynder-GX — Win32 Vulkan surface creation helper.
//
// Wraps vkCreateWin32SurfaceKHR so that psy::gpu::create_device() can obtain
// a VkSurfaceKHR from a raw HWND without exposing vulkan_win32.h to every
// consumer in the engine tree. The implementation is compiled only on Windows,
// guarded by PSYNDER_PLATFORM_WIN32.
//
// DESIGN §11.1: "Vulkan surface via vkCreateWin32SurfaceKHR. No D3D12."
//
// Usage pattern expected by lane 07 (gpu):
//
//   // Inside psy::gpu::create_device() on Windows:
//   #if PSYNDER_PLATFORM_WIN32
//   #include "platform/win32/Win32VulkanSurface.h"
//   VkSurfaceKHR surface = psynder::platform::win32::create_vulkan_surface(
//       static_cast<HWND>(desc.native_window_handle), vk_instance);
//   #endif
//
// The returned VkSurfaceKHR is owned by the psy::gpu::Device; it must be
// destroyed via vkDestroySurfaceKHR(instance, surface, nullptr) before
// vkDestroyInstance is called.
//
// On non-Windows platforms this header is a complete no-op (the outer guard
// means the header contributes zero declarations). Lane 07 must also guard its
// call site with #if PSYNDER_PLATFORM_WIN32.

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

// Win32Common.h defines WIN32_LEAN_AND_MEAN + NOMINMAX before windows.h.
#include "Win32Common.h"

// Pull in the Vulkan C headers. VK_USE_PLATFORM_WIN32_KHR activates
// VkWin32SurfaceCreateInfoKHR and PFN_vkCreateWin32SurfaceKHR.
// These must come AFTER windows.h (which Win32Common.h already pulled).
#ifndef VK_USE_PLATFORM_WIN32_KHR
#   define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace psynder::platform::win32 {

// Create a Vulkan surface for the given HWND.
//
// Parameters
//   hwnd        — a valid, non-null Win32 window handle. The window must remain
//                 alive for the lifetime of the returned surface.
//   vk_instance — the VkInstance created by psy::gpu. Must be non-null.
//
// Returns
//   A valid VkSurfaceKHR on success.
//   VK_NULL_HANDLE on failure; the failure is logged via PSY_LOG_ERROR with
//   the VkResult code.
//
// Thread safety
//   Safe to call from any thread once the HWND exists. Vulkan instance
//   functions are externally synchronised per-instance; do not call this
//   concurrently on the same VkInstance from multiple threads.
//
// NEEDS PC VALIDATION — cannot be exercised on macOS.
VkSurfaceKHR create_vulkan_surface(HWND hwnd, VkInstance vk_instance);

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
