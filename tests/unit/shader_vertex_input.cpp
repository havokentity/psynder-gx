// SPDX-License-Identifier: MIT
//
// tests/unit/shader_vertex_input.cpp
//
// Lane 09 — VertexInputDesc unit tests (closes sample01-003).
//
// Exercises the public layout descriptor on GraphicsPipelineDesc end-to-end:
//
//   * zero-init produces attr_count == 0 → default-layout fallback.
//   * a 3-attr "default-shape" layout (pos+normal+uv) sums to 32 bytes
//     and validate()s clean.
//   * an instanced layout (per-vertex Position on slot 0 + per-instance
//     InstanceModel x 4 on slot 1) tags each binding's input_rate
//     correctly.
//   * the validator rejects out-of-range buffer_slot / attr_count /
//     binding_count so the backend builders never see malformed input.
//
// No GPU is touched here — these are pure POD-layout / format-mapping
// tests against the lane 09 public surface.

#include "shader/PublicShader.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>

using psynder::shader::VertexAttrSemantic;
using psynder::shader::VertexAttrFormat;
using psynder::shader::VertexInputRate;
using psynder::shader::VertexAttr;
using psynder::shader::VertexBufferBinding;
using psynder::shader::VertexInputDesc;
using psynder::shader::GraphicsPipelineDesc;
using psynder::shader::attr_format_size;
using psynder::shader::validate;

// ─── 1. Zero-init / default-layout sentinel ────────────────────────────

TEST_CASE("VertexInputDesc zero-init means default-layout fallback",
          "[shader][vertex_input]")
{
    VertexInputDesc vi{};
    REQUIRE(vi.attr_count    == 0);
    REQUIRE(vi.binding_count == 0);
    REQUIRE(validate(vi));     // attr_count==0 is vacuously valid

    // The default-constructed GraphicsPipelineDesc must carry the same
    // zero-init sentinel — guarantees lane-08 keeps using
    // DefaultVertexLayout for every pre-lane-09 callsite.
    GraphicsPipelineDesc gp{};
    REQUIRE(gp.vertex_input.attr_count    == 0);
    REQUIRE(gp.vertex_input.binding_count == 0);
}

// ─── 2. attr_format_size for every public format ───────────────────────

TEST_CASE("attr_format_size sizes match the format-byte tables",
          "[shader][vertex_input]")
{
    CHECK(attr_format_size(VertexAttrFormat::Float32)     ==  4u);
    CHECK(attr_format_size(VertexAttrFormat::Float32x2)   ==  8u);
    CHECK(attr_format_size(VertexAttrFormat::Float32x3)   == 12u);
    CHECK(attr_format_size(VertexAttrFormat::Float32x4)   == 16u);
    CHECK(attr_format_size(VertexAttrFormat::Uint8x4Norm) ==  4u);
    CHECK(attr_format_size(VertexAttrFormat::Uint16x2)    ==  4u);
}

// ─── 3. Default-shape 3-attr layout: stride == 32 ──────────────────────

TEST_CASE("VertexInputDesc 3-attr pos+normal+uv layout sums to stride 32",
          "[shader][vertex_input]")
{
    VertexInputDesc vi{};
    vi.attr_count = 3;
    vi.attrs[0] = { VertexAttrSemantic::Position,  VertexAttrFormat::Float32x3, /*slot*/0,  /*offset*/0  };
    vi.attrs[1] = { VertexAttrSemantic::Normal,    VertexAttrFormat::Float32x3, /*slot*/0,  /*offset*/12 };
    vi.attrs[2] = { VertexAttrSemantic::TexCoord0, VertexAttrFormat::Float32x2, /*slot*/0,  /*offset*/24 };

    vi.binding_count = 1;
    vi.bindings[0]   = { /*stride*/32, VertexInputRate::Vertex };

    REQUIRE(validate(vi));

    // Sum the format sizes of the three attrs — must hit the binding stride.
    std::uint32_t computed = 0;
    for (std::uint8_t i = 0; i < vi.attr_count; ++i) {
        computed += attr_format_size(vi.attrs[i].format);
    }
    REQUIRE(computed == 32u);
    REQUIRE(computed == vi.bindings[0].stride);
}

// ─── 4. Instanced layout: per-vertex Position + per-instance InstanceModel × 4 ──

TEST_CASE("VertexInputDesc instanced layout: vertex slot 0 + instance slot 1",
          "[shader][vertex_input]")
{
    VertexInputDesc vi{};
    // Slot 0: per-vertex Position (float3 @ 0, stride 12).
    vi.attrs[0]      = { VertexAttrSemantic::Position,
                         VertexAttrFormat::Float32x3,
                         /*slot*/0, /*offset*/0 };
    // Slot 1: per-instance mat4 model matrix fanned across 4 float4 attrs
    // (the canonical Vulkan/Metal pattern for instanced mat4).  Offsets
    // 0/16/32/48; stride 64.
    vi.attrs[1]      = { VertexAttrSemantic::InstanceModel,
                         VertexAttrFormat::Float32x4,
                         /*slot*/1, /*offset*/0  };
    vi.attrs[2]      = { VertexAttrSemantic::InstanceModel,
                         VertexAttrFormat::Float32x4,
                         /*slot*/1, /*offset*/16 };
    vi.attrs[3]      = { VertexAttrSemantic::InstanceModel,
                         VertexAttrFormat::Float32x4,
                         /*slot*/1, /*offset*/32 };
    vi.attrs[4]      = { VertexAttrSemantic::InstanceModel,
                         VertexAttrFormat::Float32x4,
                         /*slot*/1, /*offset*/48 };
    vi.attr_count    = 5;

    vi.bindings[0]   = { /*stride*/12, VertexInputRate::Vertex   };
    vi.bindings[1]   = { /*stride*/64, VertexInputRate::Instance };
    vi.binding_count = 2;

    REQUIRE(validate(vi));

    // The whole point of the test: per-binding input rate semantics.
    CHECK(vi.bindings[0].input_rate == VertexInputRate::Vertex);
    CHECK(vi.bindings[1].input_rate == VertexInputRate::Instance);

    // Per-instance attribute slot stride: 4 × float4 = 64 bytes.
    std::uint32_t instance_bytes = 0;
    for (std::uint8_t i = 0; i < vi.attr_count; ++i) {
        if (vi.attrs[i].buffer_slot == 1) {
            instance_bytes += attr_format_size(vi.attrs[i].format);
        }
    }
    REQUIRE(instance_bytes == 64u);
    REQUIRE(instance_bytes == vi.bindings[1].stride);
}

// ─── 5. Validator rejects out-of-range fields ──────────────────────────

TEST_CASE("validate rejects out-of-range buffer_slot",
          "[shader][vertex_input]")
{
    VertexInputDesc vi{};
    vi.attr_count    = 1;
    vi.binding_count = 1;
    vi.bindings[0]   = { 16, VertexInputRate::Vertex };
    // buffer_slot == binding_count is one past the last valid slot.
    vi.attrs[0]      = { VertexAttrSemantic::Position,
                         VertexAttrFormat::Float32x3,
                         /*slot*/1,   // INVALID — only slot 0 is bound
                         /*offset*/0 };
    REQUIRE_FALSE(validate(vi));

    // Way past kMaxBindings (8) — also rejected.
    vi.attrs[0].buffer_slot = 250;
    REQUIRE_FALSE(validate(vi));
}

TEST_CASE("validate rejects attr_count > kMaxAttrs",
          "[shader][vertex_input]")
{
    VertexInputDesc vi{};
    vi.binding_count = 1;
    vi.bindings[0]   = { 12, VertexInputRate::Vertex };
    vi.attr_count    = static_cast<std::uint8_t>(VertexInputDesc::kMaxAttrs + 1);
    REQUIRE_FALSE(validate(vi));
}

TEST_CASE("validate rejects binding_count > kMaxBindings",
          "[shader][vertex_input]")
{
    VertexInputDesc vi{};
    vi.attr_count    = 1;
    vi.binding_count = static_cast<std::uint8_t>(VertexInputDesc::kMaxBindings + 1);
    vi.attrs[0]      = { VertexAttrSemantic::Position,
                         VertexAttrFormat::Float32x3,
                         /*slot*/0,
                         /*offset*/0 };
    REQUIRE_FALSE(validate(vi));
}

TEST_CASE("validate rejects attrs without any bindings",
          "[shader][vertex_input]")
{
    VertexInputDesc vi{};
    vi.attr_count    = 1;
    vi.binding_count = 0;
    vi.attrs[0]      = { VertexAttrSemantic::Position,
                         VertexAttrFormat::Float32x3,
                         /*slot*/0,
                         /*offset*/0 };
    REQUIRE_FALSE(validate(vi));
}

// ─── 6. POD-ness — no virtual / no dynamic alloc ───────────────────────
//
// These are static_asserts because the moment any of these conditions
// flip, the lane-04 worker-thread call path through create_graphics
// stops being callable from the JobSystem (the prompt's DOTS mandate).

static_assert(std::is_trivially_copyable_v<VertexAttr>,
              "VertexAttr must stay POD — DOTS mandate.");
static_assert(std::is_trivially_copyable_v<VertexBufferBinding>,
              "VertexBufferBinding must stay POD — DOTS mandate.");
static_assert(std::is_trivially_copyable_v<VertexInputDesc>,
              "VertexInputDesc must stay POD — DOTS mandate.");
static_assert(std::is_standard_layout_v<VertexInputDesc>,
              "VertexInputDesc must be standard-layout — ABI safety.");
