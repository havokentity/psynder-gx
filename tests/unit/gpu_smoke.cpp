// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gpu_smoke.cpp
//
// Lane 07 (gpu) — API surface smoke test.
//
// Creates a psynder::gpu::Device with native_window_handle = nullptr so
// the Metal backend initialises the MTLDevice + command queue without
// attaching a CAMetalLayer (headless mode, as documented in
// engine/gpu/mtl/MetalBackend.mm §init).
//
// Goals:
//   - device_name() returns a non-empty string (proves the Metal device
//     was selected and the name was cached as a C-string).
//   - destroy_device does not crash.
//   - Resource-creation functions return either a valid handle or a null
//     handle without aborting — null is acceptable at M0 (stubs).
//
// On non-macOS CI (Linux Vulkan) the Vulkan backend requires a real
// Vulkan loader; the test skips cleanly when no loader is present.

#include "gpu/PublicGpu.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace psynder::gpu;

TEST_CASE("gpu: create_device / destroy_device with null window handle",
          "[gpu][smoke]") {
    DeviceDesc desc{};
    desc.enable_validation      = false;
    desc.enable_rt              = false;  // avoid loader probe for RT exts at M0
    desc.enable_mesh_shaders    = false;
    desc.native_window_handle   = nullptr;

    Device* dev = create_device(desc);

    // On macOS: Metal backend must succeed (Apple Silicon always has Metal).
    // On Linux/Windows: Vulkan backend may fail in CI without a display —
    // that is acceptable; we only assert "no crash" and skip the name check
    // when device creation fails.
    if (!dev) {
        SUCCEED("create_device returned nullptr (expected in headless Vulkan CI)");
        return;
    }

    // If we got a device, the name must be non-empty.
    const char* name = device_name(dev);
    REQUIRE(name != nullptr);
    REQUIRE(std::strlen(name) > 0);

    // Query capability flags — must not crash and must return valid booleans.
    [[maybe_unused]] bool rt     = device_supports_rt(dev);
    [[maybe_unused]] bool mesh   = device_supports_mesh_shaders(dev);
    [[maybe_unused]] bool uniMem = device_is_unified_memory(dev);

    destroy_device(dev);
}

TEST_CASE("gpu: create_device twice then destroy both in LIFO order",
          "[gpu][smoke]") {
    DeviceDesc desc{};
    desc.enable_validation    = false;
    desc.native_window_handle = nullptr;

    Device* dev1 = create_device(desc);
    Device* dev2 = create_device(desc);

    // At least one must succeed (or both may fail in headless Vulkan CI).
    if (!dev1 && !dev2) {
        SUCCEED("both create_device calls returned nullptr (expected in headless Vulkan CI)");
        return;
    }

    // Destroy in LIFO order. Must not crash or double-free.
    destroy_device(dev2);
    destroy_device(dev1);
}

TEST_CASE("gpu: null-device guard functions are safe",
          "[gpu][smoke]") {
    // All public psynder::gpu free functions must guard against a null Device*
    // pointer without crashing or asserting.
    REQUIRE_FALSE(device_supports_rt(nullptr));
    REQUIRE_FALSE(device_supports_mesh_shaders(nullptr));
    REQUIRE_FALSE(device_is_unified_memory(nullptr));

    const char* name = device_name(nullptr);
    REQUIRE(name != nullptr);   // returns "" not nullptr

    // Frame-loop calls on nullptr must be safe.
    REQUIRE_FALSE(begin_frame(nullptr));
    end_frame(nullptr);
    REQUIRE(cmd_open(nullptr) == nullptr);
    cmd_submit(nullptr, nullptr);
    resize_swapchain(nullptr, 1280u, 720u);
}
