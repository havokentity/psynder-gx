// SPDX-License-Identifier: MIT
// Psynder(-GX) — Win32 platform shared bits (header-only).
//
// All Win32 sources are guarded by PSYNDER_PLATFORM_WIN32. The lane's
// CMakeLists only adds this directory when building on Windows, but we
// guard at the source level too so an out-of-band Mac/Linux compile (e.g.
// running clang-tidy across the tree) doesn't choke on windows.h.
//
// This header centralizes the Win32 + COM + DXGI + WASAPI + XInput includes
// and a few tiny helpers shared across Win32Window/Input/Present/Audio.
//
// D3D11 / D3DCompile:
//   In Psynder (CPU renderer, !PSYNDER_GX): included here for Win32Present.
//   In Psynder-GX (PSYNDER_GX defined): D3D11 is not used. The includes are
//   conditionally excluded to keep the GX build free of D3D11 headers. The
//   DXGI headers are still included for the DXGI factory types used by WASAPI
//   device enumeration.
//
// Vulkan (Win32VulkanSurface.h):
//   Win32VulkanSurface.h includes <vulkan/vulkan.h> with VK_USE_PLATFORM_WIN32_KHR.
//   That header is NOT included here to avoid polluting every Win32 TU with
//   Vulkan; only Win32VulkanSurface.{h,cpp} need it.

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#   define NOMINMAX
#endif
// Target Windows 10+ so the per-monitor-DPI APIs (SetProcessDpiAwarenessContext,
// GetDpiForSystem, AdjustWindowRectExForDpi, WM_DPICHANGED) are declared.
#ifndef _WIN32_WINNT
#   define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

// COM + WRL ComPtr (used by Win32Present in non-GX and Win32Audio always).
#include <combaseapi.h>
#include <objbase.h>
#include <wrl/client.h>

// DXGI — used by Win32Audio (IMMDevice enumeration touches DXGI types on some
// driver stacks) and Win32Present (swap chain) in non-GX builds.
#include <dxgi1_4.h>

#if !defined(PSYNDER_GX)
// D3D11 + D3DCompile are only needed for the CPU framebuffer blit (Psynder).
// In GX builds Vulkan is the only graphics API; omit to keep includes clean.
#include <d3d11.h>
#include <d3dcompiler.h>
#endif  // !PSYNDER_GX

// WASAPI audio (Win32Audio — used in both Psynder and Psynder-GX).
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>

// XInput gamepads (Win32Input — both builds).
#include <xinput.h>

#include "core/Log.h"
#include "core/Types.h"

#include <string>
#include <string_view>

namespace psynder::platform::win32 {

// Microsoft-style ComPtr alias, kept short so call sites stay readable.
template <class T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// hresult logging helper. Returns true on success, logs and returns false
// on failure. Use as `if (!psy_hr_ok(hr, "CreateSwapChain")) return false;`.
inline bool psy_hr_ok(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    PSY_LOG_ERROR("[win32] {} failed: hr=0x{:08x}", what, static_cast<u32>(hr));
    return false;
}

// Convert a UTF-8 std::string_view to a wide string for the Win32 W APIs.
// Used by window-title sets and FS helpers.
inline std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out;
    out.resize(static_cast<usize>(needed));
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        out.data(), needed);
    return out;
}

// Inverse — wide buffer to UTF-8. Used by FS helpers to return std::string.
inline std::string from_wide(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out;
    out.resize(static_cast<usize>(needed));
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    return out;
}

// One-shot COM bootstrap — multiple Init calls are fine (refcounted by Windows).
struct ComScope {
    ComScope() {
        // COINIT_MULTITHREADED matches WASAPI + DXGI threading expectations.
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // Some host already chose apartment-threaded; live with it.
            initialized_ = false;
        } else {
            initialized_ = SUCCEEDED(hr);
        }
    }
    ~ComScope() {
        if (initialized_) ::CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool initialized_ = false;
};

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
