// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gpu_texture_upload.cpp
//
// Lane 07 — verifies the M1 `TextureDesc::initial_data` upload path:
//
//   1. Upload a 4x4 RGBA8 checkerboard, read mip-0 back, compare bytes.
//   2. initial_data == nullptr — preserves the pre-M1 stub behaviour;
//      no crash, the returned Handle may be valid (the texture is just
//      uninitialised).
//   3. initial_data_size != width * height * bpp — returns an invalid
//      Handle, no crash, no double-free on the next test case.
//   4. Unsupported Format + non-null initial_data — same invalid-handle
//      contract; rejected with a logged error.
//
// Readback uses the lane-internal Backend::texture_readback_mip0 virtual
// declared in PublicGpuInternal.h. This is intentionally NOT in the
// public PublicGpu.h surface (DESIGN keeps readback out of the per-frame
// hot path; M3 will introduce the async-streaming version).
//
// Skips cleanly when create_device returns nullptr (headless Vulkan CI
// without a display / loader), matching gpu_smoke.cpp's pattern.

#include "gpu/PublicGpu.h"
#include "gpu/PublicGpuInternal.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace psynder::gpu;

namespace {

// Build a 4x4 RGBA8 checkerboard. Two colours alternate per-cell so any
// byte-swap (e.g., R↔B) would corrupt the comparison. Returns 64 bytes.
std::vector<std::uint8_t> make_4x4_rgba8_checkerboard() {
    std::vector<std::uint8_t> px(4u * 4u * 4u);
    const std::array<std::uint8_t, 4> a = {0x11, 0x22, 0x33, 0xff};
    const std::array<std::uint8_t, 4> b = {0xaa, 0xbb, 0xcc, 0xff};
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const bool flip = ((x + y) & 1u) != 0u;
            const auto& src = flip ? a : b;
            const std::size_t off = (y * 4u + x) * 4u;
            px[off + 0] = src[0];
            px[off + 1] = src[1];
            px[off + 2] = src[2];
            px[off + 3] = src[3];
        }
    }
    return px;
}

// Build an 8x4 R8 ramp. 32 bytes.
std::vector<std::uint8_t> make_8x4_r8_ramp() {
    std::vector<std::uint8_t> px(8u * 4u);
    for (std::size_t i = 0; i < px.size(); ++i) {
        px[i] = static_cast<std::uint8_t>(i * 8u);
    }
    return px;
}

// Read mip-0 back through the lane-internal backend dispatch table.
// Returns true on success.
bool read_back_texture_mip0(Texture* tex,
                            void* out_bytes,
                            std::size_t out_size)
{
    if (!tex || !tex->owner() || !tex->owner()->backend) return false;
    return tex->owner()->backend->texture_readback_mip0(tex, out_bytes, out_size);
}

} // namespace

TEST_CASE("gpu: texture initial_data round-trips through mip-0",
          "[gpu][texture][upload]") {
    DeviceDesc dd{};
    dd.enable_validation    = false;
    dd.enable_rt            = false;
    dd.enable_mesh_shaders  = false;
    dd.native_window_handle = nullptr;

    Device* dev = create_device(dd);
    if (!dev) {
        SUCCEED("create_device returned nullptr (headless CI without backend); skipping");
        return;
    }

    const auto src = make_4x4_rgba8_checkerboard();
    REQUIRE(src.size() == 4u * 4u * 4u);

    TextureDesc td{};
    td.width             = 4;
    td.height            = 4;
    td.format            = Format::Rgba8Unorm;
    td.usage             = static_cast<std::uint32_t>(TextureUsage::Sampled)
                         | static_cast<std::uint32_t>(TextureUsage::TransferDst)
                         | static_cast<std::uint32_t>(TextureUsage::TransferSrc);
    td.heap              = HeapKind::DeviceLocal;
    td.debug_name        = "test/4x4_checker";
    td.initial_data      = src.data();
    td.initial_data_size = src.size();

    Handle<Texture> tex = create_texture(dev, td);
    REQUIRE(tex);  // upload path must produce a valid handle

    std::vector<std::uint8_t> back(src.size(), 0xCDu);
    REQUIRE(read_back_texture_mip0(tex.get(), back.data(), back.size()));
    REQUIRE(back == src);

    // Drop the handle then the device so destroy_resource fires inside
    // a live backend (Vulkan needs device_ to free image+view+mem).
    tex = Handle<Texture>{};
    destroy_device(dev);
}

TEST_CASE("gpu: texture R8 initial_data round-trips through mip-0",
          "[gpu][texture][upload]") {
    DeviceDesc dd{};
    dd.enable_validation    = false;
    dd.native_window_handle = nullptr;

    Device* dev = create_device(dd);
    if (!dev) {
        SUCCEED("create_device returned nullptr; skipping");
        return;
    }

    const auto src = make_8x4_r8_ramp();
    REQUIRE(src.size() == 8u * 4u);

    TextureDesc td{};
    td.width             = 8;
    td.height            = 4;
    td.format            = Format::R8Unorm;
    td.usage             = static_cast<std::uint32_t>(TextureUsage::Sampled)
                         | static_cast<std::uint32_t>(TextureUsage::TransferDst)
                         | static_cast<std::uint32_t>(TextureUsage::TransferSrc);
    td.initial_data      = src.data();
    td.initial_data_size = src.size();

    Handle<Texture> tex = create_texture(dev, td);
    REQUIRE(tex);

    std::vector<std::uint8_t> back(src.size(), 0xCDu);
    REQUIRE(read_back_texture_mip0(tex.get(), back.data(), back.size()));
    REQUIRE(back == src);

    tex = Handle<Texture>{};
    destroy_device(dev);
}

TEST_CASE("gpu: texture create with initial_data == nullptr preserves stub behaviour",
          "[gpu][texture][upload]") {
    DeviceDesc dd{};
    dd.native_window_handle = nullptr;
    Device* dev = create_device(dd);
    if (!dev) {
        SUCCEED("create_device returned nullptr; skipping");
        return;
    }

    TextureDesc td{};
    td.width             = 16;
    td.height            = 16;
    td.format            = Format::Rgba8Unorm;
    td.usage             = static_cast<std::uint32_t>(TextureUsage::Sampled);
    td.initial_data      = nullptr;
    td.initial_data_size = 0;

    // Pre-M1, create_texture returned a bare-stub texture (handle == nil
    // / VK_NULL_HANDLE) but a valid wrapper. We preserve that here so
    // existing render lanes that allocate a TextureDesc + immediately
    // populate later (e.g. dynamic uniforms) keep working.
    Handle<Texture> tex = create_texture(dev, td);
    REQUIRE(tex);              // wrapper is valid
    REQUIRE(tex.get() != nullptr);

    // Reading back an uninitialised texture must fail cleanly (no crash).
    std::vector<std::uint8_t> back(16u * 16u * 4u, 0u);
    [[maybe_unused]] const bool ok =
        read_back_texture_mip0(tex.get(), back.data(), back.size());
    // Don't assert on the bool — backends differ on this stub: Metal
    // can't read an empty handle (returns false), Vulkan ditto. What we
    // care about is "no crash".

    tex = Handle<Texture>{};
    destroy_device(dev);
}

TEST_CASE("gpu: texture create with mismatched initial_data_size rejects",
          "[gpu][texture][upload]") {
    DeviceDesc dd{};
    dd.native_window_handle = nullptr;
    Device* dev = create_device(dd);
    if (!dev) {
        SUCCEED("create_device returned nullptr; skipping");
        return;
    }

    const auto src = make_4x4_rgba8_checkerboard();
    TextureDesc td{};
    td.width             = 4;
    td.height            = 4;
    td.format            = Format::Rgba8Unorm;
    td.usage             = static_cast<std::uint32_t>(TextureUsage::Sampled);
    td.initial_data      = src.data();
    td.initial_data_size = src.size() + 7u;   // off by 7 — must reject

    Handle<Texture> tex = create_texture(dev, td);
    REQUIRE_FALSE(tex);  // invalid Handle, no crash

    destroy_device(dev);
}

TEST_CASE("gpu: texture create with unsupported format + initial_data rejects",
          "[gpu][texture][upload]") {
    DeviceDesc dd{};
    dd.native_window_handle = nullptr;
    Device* dev = create_device(dd);
    if (!dev) {
        SUCCEED("create_device returned nullptr; skipping");
        return;
    }

    // 16x16 RGBA16F = 8 bytes/pixel (4 × FP16 = 8 B) — buffer sized
    // correctly for the format BUT the M1 upload path doesn't list
    // Rgba16Float as a supported format. Must reject before validating
    // size.  Earlier comment claimed 16 B/pixel; that was wrong
    // (Copilot PR #15 review).
    std::vector<std::uint8_t> src(16u * 16u * 8u, 0xAAu);
    TextureDesc td{};
    td.width             = 16;
    td.height            = 16;
    td.format            = Format::Rgba16Float;
    td.usage             = static_cast<std::uint32_t>(TextureUsage::Sampled);
    td.initial_data      = src.data();
    td.initial_data_size = src.size();

    Handle<Texture> tex = create_texture(dev, td);
    REQUIRE_FALSE(tex);

    destroy_device(dev);
}
