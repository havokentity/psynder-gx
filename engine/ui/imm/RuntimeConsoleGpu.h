// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ui/imm/RuntimeConsoleGpu.h
//
// Engine-rendered runtime-console and boot overlay using the shared GPU
// overlay batch. The caller owns render-pass scope.

#pragma once

#include "gpu/PublicGpu.h"
#include "math/Math.h"
#include "shader/PublicShader.h"

#include <string_view>

namespace psynder::ui::imm {

struct RuntimeOverlayGpu {
    shader::PipelineHandle        pipeline{};
    shader::PipelineHandle        intro_pipeline{};
    gpu::Handle<gpu::Buffer>      vertex_buffer;
    gpu::Handle<gpu::Buffer>      index_buffer;
    gpu::Handle<gpu::Texture>     font_texture;
    gpu::Handle<gpu::Sampler>     font_sampler;
};

struct RuntimeConsoleGpu {
    RuntimeOverlayGpu             overlay;
    double                        opened_at_seconds = 0.0;
    bool                          was_open = false;
};

struct RuntimeBootOverlayDesc {
    const char* title = "";
    const char* subtitle = "";
    const char* action = "";
    float       time_seconds = 0.0f;
};

struct RuntimeOverlayFrameDesc {
    float      viewport_width = 1.0f;
    float      viewport_height = 1.0f;
    float      time_seconds = 0.0f;
    float      effect_strength = 0.0f;
    math::Vec2 mouse_pos{};
    bool       mouse_down = false;
    bool       mouse_pressed = false;
    bool       mouse_released = false;
};

struct RuntimeOverlayFontMetrics {
    float ascent = 12.0f;
    float descent = 4.0f;
    float body_height = 16.0f;
    float line_height = 18.0f;
};

struct RuntimeOverlayButtonStyle {
    u32 bg = 0x0D1520DDu;
    u32 bg_hover = 0x143052EEu;
    u32 bg_pressed = 0x1D4D7DFFu;
    u32 border = 0x2D8CFFFFu;
    u32 text = 0x7EE787FFu;
    float text_scale = 3.25f;
};

struct RuntimeOverlayButtonResult {
    bool hovered = false;
    bool pressed = false;
    bool clicked = false;
};

struct RuntimeIntroBackgroundDesc {
    float viewport_width = 1.0f;
    float viewport_height = 1.0f;
    float time_seconds = 0.0f;
    float intensity = 1.0f;
};

bool init_runtime_overlay_gpu(gpu::Device* device, RuntimeOverlayGpu& out);
void shutdown_runtime_overlay_gpu(RuntimeOverlayGpu& overlay);
bool runtime_overlay_gpu_ready(const RuntimeOverlayGpu& overlay) noexcept;
bool begin_runtime_overlay_gpu_frame(gpu::CmdBuffer* cb,
                                     RuntimeOverlayGpu& overlay,
                                     const RuntimeOverlayFrameDesc& desc);
void end_runtime_overlay_gpu_frame();
RuntimeOverlayFontMetrics runtime_overlay_font_metrics(float scale) noexcept;
float runtime_overlay_text_width(std::string_view text, float scale) noexcept;
void runtime_overlay_filled_rect(math::Vec2 origin, math::Vec2 size, u32 rgba);
void runtime_overlay_rect_outline(math::Vec2 origin, math::Vec2 size, u32 rgba);
void runtime_overlay_text(math::Vec2 origin,
                          std::string_view text,
                          u32 rgba,
                          float scale);
RuntimeOverlayButtonResult runtime_overlay_button(math::Vec2 origin,
                                                  math::Vec2 size,
                                                  std::string_view label,
                                                  const RuntimeOverlayButtonStyle& style);
void draw_runtime_intro_background_gpu(gpu::CmdBuffer* cb,
                                       const RuntimeOverlayGpu& overlay,
                                       const RuntimeIntroBackgroundDesc& desc);

bool init_runtime_console_gpu(gpu::Device* device, RuntimeConsoleGpu& out);
void shutdown_runtime_console_gpu(RuntimeConsoleGpu& overlay);

bool runtime_console_gpu_wants_draw(RuntimeConsoleGpu& overlay);
bool runtime_console_gpu_ready(const RuntimeConsoleGpu& overlay) noexcept;

void draw_runtime_console_gpu(gpu::CmdBuffer* cb,
                              RuntimeConsoleGpu& overlay,
                              float viewport_width,
                              float viewport_height);

bool draw_runtime_overlays_gpu(gpu::CmdBuffer* cb,
                               RuntimeConsoleGpu& overlay,
                               float viewport_width,
                               float viewport_height,
                               const RuntimeBootOverlayDesc* boot_overlay,
                               const RuntimeOverlayFrameDesc* frame_desc = nullptr);

} // namespace psynder::ui::imm

namespace psynder::ui::overlay {
using imm::RuntimeBootOverlayDesc;
using imm::RuntimeConsoleGpu;
using imm::RuntimeOverlayFontMetrics;
using imm::RuntimeOverlayFrameDesc;
using imm::RuntimeOverlayGpu;
using imm::RuntimeOverlayButtonResult;
using imm::RuntimeOverlayButtonStyle;
using imm::RuntimeIntroBackgroundDesc;
using imm::begin_runtime_overlay_gpu_frame;
using imm::draw_runtime_console_gpu;
using imm::draw_runtime_intro_background_gpu;
using imm::draw_runtime_overlays_gpu;
using imm::end_runtime_overlay_gpu_frame;
using imm::init_runtime_console_gpu;
using imm::init_runtime_overlay_gpu;
using imm::runtime_console_gpu_ready;
using imm::runtime_console_gpu_wants_draw;
using imm::runtime_overlay_filled_rect;
using imm::runtime_overlay_font_metrics;
using imm::runtime_overlay_gpu_ready;
using imm::runtime_overlay_rect_outline;
using imm::runtime_overlay_text;
using imm::runtime_overlay_text_width;
using imm::runtime_overlay_button;
using imm::shutdown_runtime_console_gpu;
using imm::shutdown_runtime_overlay_gpu;
} // namespace psynder::ui::overlay
