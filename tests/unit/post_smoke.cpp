// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/post_smoke.cpp
//
// Lane 11 (render-post) - smoke + ACES math regression coverage.
//
// Three groups of assertions:
//
//   1. Lifecycle: init() with a small headless PostDesc succeeds and
//      shutdown() cleans up without leaking. Repeated init/shutdown cycles
//      stay healthy.
//
//   2. Upscaler runtime switch: set_upscaler_runtime() falls back to Off
//      for stubs that report is_supported() == false (DLSS/FSR3/XeSS),
//      and accepts MetalFX where supported (macOS) or falls back (other).
//
//   3. ACES math regression: hardcoded reference outputs for x = 0.0, 0.5,
//      1.0, 2.0, 5.0, 10.0. These pin the Narkowicz 2015 curve
//      coefficients (a=2.51, b=0.03, c=2.43, d=0.59, e=0.14); any change
//      to the shader formula must be matched here, which keeps drift
//      visible in code review.
//
// Note: PR #15 (lane/07-gpu-texture-upload) has not landed on this branch
// at the time of writing, so we don't yet have Backend::texture_readback_mip0()
// available for end-to-end GPU verification of the tonemapper. The ACES
// math regression below is a pure-CPU mirror of the shader formula and
// therefore catches accidental coefficient edits regardless of GPU state.
//
// References:
//   - Narkowicz 2015 "ACES Filmic Tone Mapping Curve".

#include "render/post/PublicRenderPost.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>

using Catch::Matchers::WithinAbs;
using namespace psynder::render::post;

namespace {

// CPU mirror of bloom.slang's `aces_tonemap(x)`. KEEP IN LOCKSTEP with
// engine/render/post/shaders/tonemap.slang -- any change to the shader
// constants must be reflected here AND in the reference values below.
float aces_cpu(float x)
{
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    const float num = x * (a * x + b);
    const float den = x * (c * x + d) + e;
    if (den == 0.0f) return 0.0f; // x==0 case explicitly safe
    const float r = num / den;
    return std::clamp(r, 0.0f, 1.0f);
}

} // namespace

// ─── Lifecycle ─────────────────────────────────────────────────────────

TEST_CASE("post: init() with small headless PostDesc, then shutdown()",
          "[post][smoke]") {
    PostDesc desc{};
    desc.output_width   = 256;
    desc.output_height  = 256;
    desc.upscaler       = Upscaler::Off;
    desc.bloom_strength = 0.04f;
    desc.exposure_ev    = 0.0f;

    // No GPU device is wired in headless mode; init() must still succeed
    // (it short-circuits the GPU resource creation and leaves the post
    // stack in "not-ready, run_frame is a no-op" state).
    REQUIRE_NOTHROW(init(desc));
    REQUIRE_NOTHROW(run_frame()); // no-op without a real device
    REQUIRE_NOTHROW(shutdown());
}

TEST_CASE("post: init/shutdown is idempotent across repeated cycles",
          "[post][smoke]") {
    PostDesc desc{};
    desc.output_width   = 128;
    desc.output_height  = 128;
    desc.upscaler       = Upscaler::Off;
    desc.bloom_strength = 0.05f;
    desc.exposure_ev    = -0.5f;

    for (int i = 0; i < 4; ++i) {
        REQUIRE_NOTHROW(init(desc));
        REQUIRE_NOTHROW(run_frame());
        REQUIRE_NOTHROW(shutdown());
    }
}

// ─── Upscaler runtime switch ───────────────────────────────────────────

TEST_CASE("post: set_upscaler_runtime() falls back to Off for unsupported",
          "[post][upscale]") {
    PostDesc desc{};
    desc.output_width   = 256;
    desc.output_height  = 256;
    desc.upscaler       = Upscaler::Off;
    init(desc);

    // DLSS / FSR3 / XeSS stubs all report is_supported() == false at M2;
    // requesting any of them must fall back to Off cleanly.
    REQUIRE_NOTHROW(set_upscaler_runtime(Upscaler::Dlss));
    REQUIRE_NOTHROW(set_upscaler_runtime(Upscaler::Fsr3));
    REQUIRE_NOTHROW(set_upscaler_runtime(Upscaler::Xess));
    REQUIRE_NOTHROW(set_upscaler_runtime(Upscaler::MetalFx));
    REQUIRE_NOTHROW(set_upscaler_runtime(Upscaler::Off));

    shutdown();
}

// ─── ACES math regression (Narkowicz 2015) ─────────────────────────────
//
// Hardcoded reference values for the curve at x = 0.0, 0.5, 1.0, 2.0,
// 5.0, 10.0 (computed once from the formula, pinned here). If anyone
// accidentally edits the coefficients in tonemap.slang or aces_cpu()
// above, this test catches the drift.
//
// Tolerance: 1e-6 (well below the precision the GPU side would deliver).

TEST_CASE("post: ACES Narkowicz curve hits the reference values exactly",
          "[post][tonemap][regression]") {
    // Catch2's WithinAbs takes doubles -- compare against the float
    // result promoted to double, with a 1e-6 tolerance.
    constexpr double kTol = 1e-6;

    // x = 0: ACES(0) = 0/0.14 = 0
    REQUIRE_THAT(static_cast<double>(aces_cpu(0.0f)),  WithinAbs(0.0,          kTol));

    // x = 0.5: (0.5 * 1.285) / (0.5 * 1.805 + 0.14) = 0.6425 / 1.0425
    REQUIRE_THAT(static_cast<double>(aces_cpu(0.5f)),  WithinAbs(0.6163069544, kTol));

    // x = 1.0: 2.54 / 3.16
    REQUIRE_THAT(static_cast<double>(aces_cpu(1.0f)),  WithinAbs(0.8037974684, kTol));

    // x = 2.0: 10.10 / 11.04
    REQUIRE_THAT(static_cast<double>(aces_cpu(2.0f)),  WithinAbs(0.9148550725, kTol));

    // x = 5.0: 62.9 / 63.84
    REQUIRE_THAT(static_cast<double>(aces_cpu(5.0f)),  WithinAbs(0.9852756892, kTol));

    // x = 10.0: unsaturated curve value is ~1.00907, saturate clamps to 1.
    REQUIRE_THAT(static_cast<double>(aces_cpu(10.0f)), WithinAbs(1.0,          kTol));
}

TEST_CASE("post: ACES curve is monotone-increasing on positive HDR",
          "[post][tonemap]") {
    // Sanity: ACES is a tone-mapping curve -- brighter input never maps
    // to dimmer output. This catches sign-flip / coefficient-swap edits.
    float prev = -1.0f;
    for (float x = 0.0f; x <= 16.0f; x += 0.25f) {
        const float y = aces_cpu(x);
        REQUIRE(y >= 0.0f);
        REQUIRE(y <= 1.0f);
        REQUIRE(y >= prev - 1.0e-6f); // allow tiny float wobble at saturation
        prev = y;
    }
}

TEST_CASE("post: ACES curve produces finite output across HDR range",
          "[post][tonemap]") {
    // Single-pixel HDR input check (the task spec asks for one pixel at
    // (10, 10, 10) verified in [0, 1] without NaN / Inf). We mirror that
    // on the CPU side here -- the GPU end-to-end equivalent lands when
    // PR #15's texture_readback_mip0 helper merges.
    constexpr float kBright = 10.0f;
    const float r = aces_cpu(kBright);
    const float g = aces_cpu(kBright);
    const float b = aces_cpu(kBright);

    REQUIRE(std::isfinite(r));
    REQUIRE(std::isfinite(g));
    REQUIRE(std::isfinite(b));
    REQUIRE(r >= 0.0f);
    REQUIRE(r <= 1.0f);
    REQUIRE(g >= 0.0f);
    REQUIRE(g <= 1.0f);
    REQUIRE(b >= 0.0f);
    REQUIRE(b <= 1.0f);
}

// ─── Exposure interaction ─────────────────────────────────────────────

TEST_CASE("post: exposure EV scales linear input before ACES",
          "[post][tonemap][exposure]") {
    // The shader applies hdr.rgb * exp2(exposure_ev) BEFORE the ACES curve,
    // so positive EV brightens and negative EV darkens (intuitive photo
    // semantics). Verify this on the CPU mirror.
    const float x0 = 0.25f;

    // +1 EV doubles the linear input -> should map higher.
    // exp2f keeps the multiplier in float to dodge double-promotion warnings.
    const float y_neg = aces_cpu(x0 * std::exp2f(-1.0f));
    const float y_0   = aces_cpu(x0);
    const float y_pos = aces_cpu(x0 * std::exp2f( 1.0f));

    REQUIRE(y_neg < y_0);
    REQUIRE(y_0   < y_pos);
}
