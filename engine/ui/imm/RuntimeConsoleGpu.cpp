// SPDX-License-Identifier: MIT OR Apache-2.0

#include "RuntimeConsoleGpu.h"

#include "Imm.h"
#include "core/console/RuntimeConsole.h"
#include "detail/Font.h"
#include "detail/GpuBatch.h"

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wold-style-cast"
#  pragma clang diagnostic ignored "-Wsign-conversion"
#  pragma clang diagnostic ignored "-Wconversion"
#  pragma clang diagnostic ignored "-Wdouble-promotion"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::ui::imm {

namespace {

constexpr u32 kPanelBg      = 0x070A0FFFu;
constexpr u32 kPanelBorder  = 0x2D8CFFFFu;
constexpr u32 kPromptBg     = 0x0D1520FFu;
constexpr u32 kOutputText   = 0xDDE7F2FFu;
constexpr u32 kPromptText   = 0x7EE787FFu;
constexpr u32 kMutedText    = 0x7D8EA3FFu;
constexpr u32 kPopupBg      = 0x111923FFu;
constexpr u32 kPopupSelect  = 0x244C79FFu;
constexpr u32 kPopupKind    = 0x78A8D8FFu;
constexpr u32 kPopupValue   = 0xF1C56BFFu;
constexpr u32 kBootTitle    = 0xF6FBFFFFu;
constexpr u32 kBootSubtle   = 0x95A8BDFFu;
constexpr u32 kBootAccent   = 0x7EE787FFu;
constexpr f32 kPad          = 12.0f;
constexpr f32 kTextScale    = 2.0f;
constexpr f32 kFontBakePx   = 32.0f;
constexpr u32 kFontAtlasW   = 1024;
constexpr u32 kFontAtlasH   = 1024;
constexpr std::size_t kMaxConsoleChars = 96;
constexpr std::size_t kMaxPopupChars = 72;
constexpr double kOpenAnimSeconds = 0.16;

struct SdfGlyph {
    f32 u0 = 0.0f;
    f32 v0 = 0.0f;
    f32 u1 = 0.0f;
    f32 v1 = 0.0f;
    f32 xoff = 0.0f;
    f32 yoff = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;
    f32 advance = 0.0f;
};

struct FontAtlasState {
    std::array<SdfGlyph, 96> glyphs{};
    f32 ascent_px = 0.0f;
    f32 descent_px = 0.0f;
    f32 line_gap_px = 0.0f;
    bool ready = false;
};

struct ScaledFontMetrics {
    f32 ascent = 12.0f;
    f32 descent = 4.0f;
    f32 body_height = 16.0f;
    f32 line_height = 18.0f;
};

struct RuntimeOverlayCanvasState {
    math::Vec2 mouse_pos{};
    bool mouse_down = false;
    bool mouse_pressed = false;
    bool mouse_released = false;
    u64 active_id = 0;
};

FontAtlasState& font_atlas() noexcept {
    static FontAtlasState s;
    return s;
}

RuntimeOverlayCanvasState& canvas_state() noexcept {
    static RuntimeOverlayCanvasState s;
    return s;
}

u64 hash_button_id(math::Vec2 origin, math::Vec2 size, std::string_view label) noexcept {
    u64 h = 1469598103934665603ull;
    auto mix_u32 = [&](u32 v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix_u32(static_cast<u32>(std::lround(origin.x * 16.0f)));
    mix_u32(static_cast<u32>(std::lround(origin.y * 16.0f)));
    mix_u32(static_cast<u32>(std::lround(size.x * 16.0f)));
    mix_u32(static_cast<u32>(std::lround(size.y * 16.0f)));
    for (const char c : label) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

bool point_in_rect(math::Vec2 p, math::Vec2 origin, math::Vec2 size) noexcept {
    return p.x >= origin.x && p.y >= origin.y &&
           p.x < origin.x + size.x && p.y < origin.y + size.y;
}

std::vector<u8> read_file_bytes(const char* path) {
    if (!path || !*path) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) return {};
    in.seekg(0, std::ios::beg);
    std::vector<u8> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

bool build_font_atlas(std::vector<u8>& pixels) {
    pixels.assign(static_cast<std::size_t>(kFontAtlasW) * kFontAtlasH, 0u);
    auto& atlas = font_atlas();
    atlas = {};
#if defined(PSYNDER_GX_UI_OVERLAY_FONT_PATH)
    std::vector<u8> font = read_file_bytes(PSYNDER_GX_UI_OVERLAY_FONT_PATH);
#elif defined(PSYNDER_GX_UI_IMM_FONT_PATH)
    std::vector<u8> font = read_file_bytes(PSYNDER_GX_UI_IMM_FONT_PATH);
#else
    std::vector<u8> font;
#endif
    if (font.empty()) {
        pixels[0] = 255u;
        return false;
    }

    pixels[0] = 255u;

    stbtt_fontinfo info{};
    if (stbtt_InitFont(&info, font.data(), 0) == 0) {
        atlas.ready = false;
        return false;
    }

    constexpr int kPadding = 8;
    constexpr unsigned char kOnEdge = 128;
    constexpr float kDistScale = 48.0f;
    const float font_scale = stbtt_ScaleForPixelHeight(&info, kFontBakePx);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    atlas.ascent_px = static_cast<f32>(ascent) * font_scale;
    atlas.descent_px = static_cast<f32>(descent) * font_scale;
    atlas.line_gap_px = static_cast<f32>(line_gap) * font_scale;
    int pen_x = 1;
    int pen_y = 1;
    int row_h = 0;

    for (int cp = 32; cp < 128; ++cp) {
        int advance = 0;
        int lsb = 0;
        stbtt_GetCodepointHMetrics(&info, cp, &advance, &lsb);
        SdfGlyph glyph{};
        glyph.advance = static_cast<f32>(advance) * font_scale;
        const std::size_t glyph_index = static_cast<std::size_t>(cp - 32);

        if (cp == 32) {
            atlas.glyphs[glyph_index] = glyph;
            continue;
        }

        int w = 0;
        int h = 0;
        int xoff = 0;
        int yoff = 0;
        unsigned char* sdf = stbtt_GetCodepointSDF(&info,
                                                   font_scale,
                                                   cp,
                                                   kPadding,
                                                   kOnEdge,
                                                   kDistScale,
                                                   &w,
                                                   &h,
                                                   &xoff,
                                                   &yoff);
        if (!sdf || w <= 0 || h <= 0) {
            if (sdf) {
                stbtt_FreeSDF(sdf, nullptr);
            }
            atlas.glyphs[glyph_index] = glyph;
            continue;
        }

        if (pen_x + w + 1 > static_cast<int>(kFontAtlasW)) {
            pen_x = 1;
            pen_y += row_h + 1;
            row_h = 0;
        }
        if (pen_y + h + 1 > static_cast<int>(kFontAtlasH)) {
            stbtt_FreeSDF(sdf, nullptr);
            atlas.ready = false;
            return false;
        }

        for (int row = 0; row < h; ++row) {
            std::memcpy(&pixels[static_cast<std::size_t>(pen_y + row) * kFontAtlasW
                                + static_cast<std::size_t>(pen_x)],
                        &sdf[row * w],
                        static_cast<std::size_t>(w));
        }

        glyph.u0 = static_cast<f32>(pen_x) / static_cast<f32>(kFontAtlasW);
        glyph.v0 = static_cast<f32>(pen_y) / static_cast<f32>(kFontAtlasH);
        glyph.u1 = static_cast<f32>(pen_x + w) / static_cast<f32>(kFontAtlasW);
        glyph.v1 = static_cast<f32>(pen_y + h) / static_cast<f32>(kFontAtlasH);
        glyph.xoff = static_cast<f32>(xoff);
        glyph.yoff = static_cast<f32>(yoff);
        glyph.w = static_cast<f32>(w);
        glyph.h = static_cast<f32>(h);
        atlas.glyphs[glyph_index] = glyph;

        pen_x += w + 1;
        row_h = std::max(row_h, h);
        stbtt_FreeSDF(sdf, nullptr);
    }

    font_atlas().ready = true;
    return font_atlas().ready;
}

f32 display_scale_for_text_scale(f32 scale) noexcept {
    return (scale * 8.0f) / kFontBakePx;
}

ScaledFontMetrics font_metrics_scaled(f32 scale = kTextScale) noexcept {
    if (!font_atlas().ready) {
        const f32 body = static_cast<f32>(detail::kGlyphHeight) * scale;
        return {body, 0.0f, body, body + std::max(1.0f, scale)};
    }

    const f32 s = display_scale_for_text_scale(scale);
    const auto& atlas = font_atlas();
    const f32 ascent = atlas.ascent_px * s;
    const f32 descent = -atlas.descent_px * s;
    const f32 gap = std::max(0.0f, atlas.line_gap_px * s);
    const f32 body = std::ceil(ascent + descent);
    const f32 line = std::ceil(body + std::clamp(gap, 1.0f, 3.0f));
    return {ascent, descent, body, line};
}

std::string clip_line(std::string_view text, std::size_t max_chars) {
    if (text.size() <= max_chars) return std::string{text};
    return std::string{text.substr(text.size() - max_chars)};
}

void draw_text_scaled(math::Vec2 pos,
                      std::string_view text,
                      u32 colour,
                      f32 scale = kTextScale) {
    if (font_atlas().ready) {
        const f32 s = display_scale_for_text_scale(scale);
        const ScaledFontMetrics metrics = font_metrics_scaled(scale);
        f32 pen_x = pos.x;
        const f32 baseline_y = pos.y + metrics.ascent;
        for (const char ch : text) {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (uch < 32 || uch > 127) {
                pen_x += 12.0f * s;
                continue;
            }
            const SdfGlyph& g = font_atlas().glyphs[uch - 32];
            if (g.w > 0.0f && g.h > 0.0f) {
                const f32 x0 = pen_x + g.xoff * s;
                const f32 y0 = baseline_y + g.yoff * s;
                const f32 x1 = x0 + g.w * s;
                const f32 y1 = y0 + g.h * s;
                detail::batch().push_textured_quad(x0, y0, x1, y1,
                                                   g.u0, g.v0, g.u1, g.v1,
                                                   colour);
            }
            pen_x += g.advance * s;
        }
        return;
    }

    f32 pen = pos.x;
    for (const char ch : text) {
        const detail::Glyph& glyph = detail::glyph_for(ch);
        for (u32 row = 0; row < detail::kGlyphHeight; ++row) {
            const u8 bits = glyph.rows[row];
            if (bits == 0U) continue;
            for (u32 col = 0; col < detail::kGlyphWidth; ++col) {
                const u8 mask = static_cast<u8>(
                    1U << (detail::kGlyphWidth - 1U - col));
                if ((bits & mask) == 0) continue;
                const f32 x0 = pen + static_cast<f32>(col) * scale;
                const f32 y0 = pos.y + static_cast<f32>(row) * scale;
                detail::batch().push_quad(x0, y0, x0 + scale, y0 + scale, colour);
            }
        }
        pen += static_cast<f32>(detail::kCellWidth) * scale;
    }
}

f32 text_width_scaled(std::string_view text, f32 scale = kTextScale) {
    if (font_atlas().ready) {
        const f32 s = display_scale_for_text_scale(scale);
        f32 width = 0.0f;
        for (const char ch : text) {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (uch < 32 || uch > 127) {
                width += 12.0f * s;
                continue;
            }
            width += font_atlas().glyphs[uch - 32].advance * s;
        }
        return width;
    }
    return static_cast<f32>(text.size() * detail::kCellWidth) * scale;
}

f32 console_cell_width() {
    return font_atlas().ready
        ? std::max(7.0f, font_atlas().glyphs['M' - 32].advance *
                            display_scale_for_text_scale(kTextScale))
        : static_cast<f32>(detail::kCellWidth) * kTextScale;
}

const char* completion_kind_label(console::CompletionKind kind) {
    switch (kind) {
        case console::CompletionKind::Cvar:    return "cvar";
        case console::CompletionKind::Command: return "cmd";
        case console::CompletionKind::Value:   return "val";
    }
    return "";
}

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

float sync_open_animation(RuntimeConsoleGpu& overlay) {
    const bool open = psynder::console::runtime_console_open();
    const double now = now_seconds();
    if (open && !overlay.was_open) {
        overlay.opened_at_seconds = now;
    } else if (!open) {
        overlay.opened_at_seconds = 0.0;
    }
    overlay.was_open = open;
    if (!open) {
        return 0.0f;
    }

    const double elapsed = now - overlay.opened_at_seconds;
    float t = static_cast<float>(elapsed / kOpenAnimSeconds);
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void draw_console_contents(float width, float height, float open_t) {
    if (open_t <= 0.0f) return;

    const ScaledFontMetrics text_metrics = font_metrics_scaled(kTextScale);
    const f32 text_body_h = text_metrics.body_height;
    const f32 line_h = text_metrics.line_height;
    const f32 panel_w = std::max(320.0f, width - 32.0f);
    const f32 panel_h = std::clamp(height * 0.42f, 180.0f, 360.0f);
    const f32 slide_y = (1.0f - open_t) * -(panel_h + 24.0f);
    const math::Vec2 panel_pos{16.0f, 16.0f + slide_y};
    const math::Vec2 panel_size{panel_w, panel_h};

    filled_rect(panel_pos, panel_size, kPanelBg);
    rect_outline(panel_pos, panel_size, kPanelBorder);

    const f32 prompt_h = text_body_h + 12.0f;
    const math::Vec2 prompt_pos{panel_pos.x + 1.0f,
                                panel_pos.y + panel_h - prompt_h - 1.0f};
    filled_rect(prompt_pos, {panel_w - 2.0f, prompt_h}, kPromptBg);

    const std::string_view line = psynder::console::runtime_console_line();
    const std::size_t cursor =
        std::min(psynder::console::runtime_console_cursor(), line.size());
    const std::size_t max_line_chars = kMaxConsoleChars - 2u;
    const std::size_t visible_start =
        (cursor > max_line_chars) ? (cursor - max_line_chars) : 0u;
    const std::size_t visible_count =
        std::min(max_line_chars, line.size() - visible_start);
    const std::string visible_line{
        line.substr(visible_start, visible_count)};

    const math::Vec2 prompt_text_pos{
        panel_pos.x + kPad,
        prompt_pos.y + std::floor((prompt_h - text_body_h) * 0.5f)};
    draw_text_scaled(prompt_text_pos, "> ", kPromptText);

    const std::size_t sel_start =
        std::min(psynder::console::runtime_console_selection_start(), line.size());
    const std::size_t sel_end =
        std::min(psynder::console::runtime_console_selection_end(), line.size());
    const std::size_t visible_end = visible_start + visible_count;
    const f32 text_cell_w = console_cell_width();
    if (sel_end > sel_start && sel_end > visible_start && sel_start < visible_end) {
        const std::size_t a = std::max(sel_start, visible_start);
        const std::size_t b = std::min(sel_end, visible_end);
        const f32 x = prompt_text_pos.x + 2.0f * text_cell_w +
            static_cast<f32>(a - visible_start) * text_cell_w;
        filled_rect({x, prompt_text_pos.y},
                    {static_cast<f32>(b - a) * text_cell_w, text_body_h},
                    kPopupSelect);
    }
    draw_text_scaled({prompt_text_pos.x + 2.0f * text_cell_w, prompt_text_pos.y},
                     visible_line,
                     kPromptText);

    const std::size_t cursor_visible =
        std::clamp(cursor, visible_start, visible_end) - visible_start;
    const f32 caret_x = prompt_text_pos.x + 2.0f * text_cell_w +
        static_cast<f32>(cursor_visible) * text_cell_w;
    filled_rect({caret_x, prompt_text_pos.y}, {2.0f, text_body_h}, kPromptText);

    const auto& output = psynder::console::runtime_console_output();
    if (output.empty()) {
        draw_text_scaled({panel_pos.x + kPad, panel_pos.y + kPad},
                         "Psynder-GX console",
                         kMutedText);
    } else {
        const f32 top = panel_pos.y + kPad;
        f32 y = prompt_pos.y - line_h - 8.0f;
        const std::size_t scroll = std::min(psynder::console::runtime_console_output_scroll(),
                                            output.size() - 1u);
        for (std::size_t i = output.size() - scroll; i > 0 && y >= top; --i) {
            draw_text_scaled({panel_pos.x + kPad, y},
                             clip_line(output[i - 1], kMaxConsoleChars),
                             kOutputText);
            y -= line_h;
        }

        if (output.size() > 1u) {
            const f32 rail_h = prompt_pos.y - panel_pos.y - 2.0f * kPad;
            const f32 rail_x = panel_pos.x + panel_w - 8.0f;
            filled_rect({rail_x, panel_pos.y + kPad}, {2.0f, rail_h}, 0x324055FFu);
            const f32 visible_rows =
                std::max(1.0f, std::floor(rail_h / line_h));
            const f32 thumb_h =
                std::clamp(rail_h * (visible_rows / static_cast<f32>(output.size())),
                           12.0f,
                           rail_h);
            const f32 denom = static_cast<f32>(output.size() - 1u);
            const f32 t = 1.0f - static_cast<f32>(scroll) / denom;
            const f32 thumb_y = panel_pos.y + kPad + t * (rail_h - thumb_h);
            filled_rect({rail_x - 3.0f, thumb_y}, {7.0f, thumb_h}, kPanelBorder);
        }
    }

    const auto completions = psynder::console::runtime_console_completions();
    if (!completions.empty()) {
        const std::size_t selected =
            std::min(psynder::console::runtime_console_completion_selected(),
                     completions.size() - 1u);
        const std::size_t popup_rows = std::min<std::size_t>(8, completions.size());
        const f32 row_h = line_h + 4.0f;
        const f32 popup_h = static_cast<f32>(popup_rows) * row_h + 6.0f;
        const f32 popup_w = std::min(panel_w - 2.0f * kPad, 720.0f);
        const f32 popup_y = std::max(panel_pos.y + kPad,
                                     prompt_pos.y - popup_h - 4.0f);
        const math::Vec2 popup_pos{panel_pos.x + kPad, popup_y};
        filled_rect(popup_pos, {popup_w, popup_h}, kPopupBg);
        rect_outline(popup_pos, {popup_w, popup_h}, kPanelBorder);

        std::size_t first = 0;
        if (selected >= popup_rows) {
            first = selected - popup_rows + 1u;
        }
        for (std::size_t row = 0; row < popup_rows; ++row) {
            const std::size_t idx = first + row;
            if (idx >= completions.size()) break;
            const auto& c = completions[idx];
            const f32 y = popup_pos.y + 4.0f + static_cast<f32>(row) * row_h;
            if (idx == selected) {
                filled_rect({popup_pos.x + 2.0f, y - 2.0f},
                            {popup_w - 4.0f, row_h},
                            kPopupSelect);
            }
            std::string row_text = c.name;
            if (!c.value.empty()) {
                row_text += "  ";
                row_text += c.value;
            }
            draw_text_scaled({popup_pos.x + 8.0f, y},
                             clip_line(row_text, kMaxPopupChars),
                             idx == selected ? 0xFFFFFFFFu : kOutputText);
            draw_text_scaled({popup_pos.x + popup_w - 54.0f, y},
                             completion_kind_label(c.kind),
                             c.value == "current" ? kPopupValue : kPopupKind);
        }
    }
}

bool draw_boot_overlay(float width, float height, const RuntimeBootOverlayDesc& desc) {
    const f32 t = desc.time_seconds;
    const f32 cx = width * 0.5f;
    const f32 cy = height * 0.48f;

    runtime_overlay_filled_rect({0.0f, 0.0f}, {width, height}, 0x02060D66u);

    for (u32 i = 0; i < 7; ++i) {
        const f32 fi = static_cast<f32>(i);
        const f32 phase = t * (0.35f + fi * 0.025f) + fi * 0.71f;
        const f32 w = width * (0.18f + 0.034f * fi);
        const f32 h = 1.0f + std::fmod(fi, 2.0f);
        const f32 x = cx - w * 0.5f + std::sin(phase) * width * 0.035f;
        const f32 y = cy - 165.0f + fi * 50.0f + std::cos(phase * 1.37f) * 7.0f;
        const u32 a = static_cast<u32>(0x0Cu + i * 0x05u);
        const u32 colour = (0x1Au << 24) | (0xA5u << 16) | (0xFFu << 8) | a;
        runtime_overlay_filled_rect({x, y}, {w, h}, colour);
    }

    const f32 pulse = 0.5f + 0.5f * std::sin(t * 2.2f);
    const f32 mark_w = 178.0f + pulse * 24.0f;
    const f32 mark_h = 178.0f + pulse * 24.0f;
    runtime_overlay_rect_outline({cx - mark_w * 0.5f, cy - mark_h * 0.5f - 122.0f},
                                 {mark_w, mark_h},
                                 0x2D8CFF66u);
    runtime_overlay_rect_outline({cx - mark_w * 0.5f + 14.0f,
                                  cy - mark_h * 0.5f - 108.0f},
                                 {mark_w - 24.0f, mark_h - 24.0f},
                                 0x7EE78766u);

    constexpr std::string_view brand = "PsyArcadeGX";
    const f32 brand_scale = 3.25f;
    const f32 brand_w = runtime_overlay_text_width(brand, brand_scale);
    runtime_overlay_text({cx - brand_w * 0.5f, cy - 112.0f},
                         brand,
                         0x7EE787DDu,
                         brand_scale);

    const std::string_view title = desc.title ? std::string_view{desc.title} : std::string_view{};
    const std::string_view subtitle =
        desc.subtitle ? std::string_view{desc.subtitle} : std::string_view{};
    const std::string_view action =
        desc.action ? std::string_view{desc.action} : std::string_view{};

    const f32 title_scale = 7.0f;
    const f32 subtitle_scale = 3.0f;
    const f32 action_scale = 3.25f;
    const f32 title_w = runtime_overlay_text_width(title, title_scale);
    const f32 subtitle_w = runtime_overlay_text_width(subtitle, subtitle_scale);
    const f32 action_w = runtime_overlay_text_width(action, action_scale);

    runtime_overlay_text({cx - title_w * 0.5f, cy + 22.0f},
                         title,
                         kBootTitle,
                         title_scale);
    runtime_overlay_text({cx - subtitle_w * 0.5f, cy + 126.0f},
                         subtitle,
                         kBootSubtle,
                         subtitle_scale);
    const math::Vec2 action_pos{cx - action_w * 0.5f, cy + 218.0f};
    bool action_clicked = false;
    if (!action.empty()) {
        const RuntimeOverlayFontMetrics action_metrics =
            runtime_overlay_font_metrics(action_scale);
        const math::Vec2 button_pos{
            action_pos.x - 22.0f,
            action_pos.y - std::floor((44.0f - action_metrics.body_height) * 0.5f)};
        const math::Vec2 button_size{action_w + 44.0f, 44.0f};
        RuntimeOverlayButtonStyle style{};
        style.bg = 0x07101866u;
        style.bg_hover = 0x112B44BBu;
        style.bg_pressed = 0x1D4D7DFFu;
        style.border = 0x7EE787FFu;
        style.text = kBootAccent;
        style.text_scale = action_scale;
        action_clicked =
            runtime_overlay_button(button_pos, button_size, action, style).clicked;
    }
    return action_clicked;
}

} // namespace

bool init_runtime_overlay_gpu(gpu::Device* device, RuntimeOverlayGpu& out) {
    namespace g = psynder::gpu;
    namespace sh = psynder::shader;
    if (!device) return false;

    g::BufferDesc vb{};
    vb.size_bytes = sizeof(detail::ImmVertex) * detail::kImmMaxVertices;
    vb.usage = static_cast<std::uint32_t>(g::BufferUsage::Vertex);
    vb.heap = g::HeapKind::HostVisible;
    vb.debug_name = "runtime_overlay_vb";
    out.vertex_buffer = g::create_buffer(device, vb);

    g::BufferDesc ib{};
    ib.size_bytes = sizeof(u32) * detail::kImmMaxIndices;
    ib.usage = static_cast<std::uint32_t>(g::BufferUsage::Index);
    ib.heap = g::HeapKind::HostVisible;
    ib.debug_name = "runtime_overlay_ib";
    out.index_buffer = g::create_buffer(device, ib);

    if (!out.vertex_buffer || !out.index_buffer) {
        std::fputs("[ui_overlay] runtime overlay GPU buffer creation failed\n", stderr);
        return false;
    }

    std::vector<u8> atlas_pixels;
    const bool font_ok = build_font_atlas(atlas_pixels);
    if (!font_ok) {
        std::fputs("[ui_overlay] JetBrains Mono SDF font atlas unavailable\n",
                   stderr);
    }
    g::TextureDesc font_tex{};
    font_tex.width = kFontAtlasW;
    font_tex.height = kFontAtlasH;
    font_tex.format = g::Format::R8Unorm;
    font_tex.usage = static_cast<std::uint32_t>(g::TextureUsage::Sampled) |
                     static_cast<std::uint32_t>(g::TextureUsage::TransferDst);
    font_tex.heap = g::HeapKind::DeviceLocal;
    font_tex.debug_name = "runtime_overlay_font_sdf_atlas";
    font_tex.initial_data = atlas_pixels.data();
    font_tex.initial_data_size = atlas_pixels.size();
    out.font_texture = g::create_texture(device, font_tex);
    out.font_sampler = g::create_sampler(device);
    if (!out.font_texture || !out.font_sampler) {
        std::fputs("[ui_overlay] runtime overlay font texture creation failed\n", stderr);
        return false;
    }

    sh::GraphicsPipelineDesc gp{};
#if defined(PSYNDER_GX_UI_OVERLAY_SHADER_PATH)
    gp.slang_path = PSYNDER_GX_UI_OVERLAY_SHADER_PATH;
#elif defined(PSYNDER_GX_UI_IMM_SHADER_PATH)
    gp.slang_path = PSYNDER_GX_UI_IMM_SHADER_PATH;
#else
    gp.slang_path = "engine/ui/imm/shaders/imm_overlay.slang";
#endif
    gp.entry_point_vs = "vs_main";
    gp.entry_point_fs = "fs_main";
    gp.color_format_count = 1;
    gp.color_formats[0] = static_cast<std::uint8_t>(g::Format::Bgra8Srgb);
    gp.depth_format = 0;
    gp.enable_depth_write = false;
    gp.enable_blend = true;

    sh::VertexInputDesc& vi = gp.vertex_input;
    vi.binding_count = 1;
    vi.bindings[0].stride = static_cast<std::uint16_t>(sizeof(detail::ImmVertex));
    vi.bindings[0].input_rate = sh::VertexInputRate::Vertex;
    vi.attr_count = 3;
    vi.attrs[0] = {sh::VertexAttrSemantic::Position,
                   sh::VertexAttrFormat::Float32x2,
                   0,
                   0};
    vi.attrs[1] = {sh::VertexAttrSemantic::TexCoord0,
                   sh::VertexAttrFormat::Float32x2,
                   0,
                   static_cast<std::uint16_t>(offsetof(detail::ImmVertex, u))};
    vi.attrs[2] = {sh::VertexAttrSemantic::Color0,
                   sh::VertexAttrFormat::Uint8x4Norm,
                   0,
                   static_cast<std::uint16_t>(offsetof(detail::ImmVertex, rgba))};

    out.pipeline = sh::create_graphics(gp);
    if (!out.pipeline.valid()) {
        std::fputs("[ui_overlay] runtime overlay pipeline creation failed\n", stderr);
        return false;
    }

    sh::GraphicsPipelineDesc intro_gp{};
#if defined(PSYNDER_GX_UI_INTRO_SHADER_PATH)
    intro_gp.slang_path = PSYNDER_GX_UI_INTRO_SHADER_PATH;
#else
    intro_gp.slang_path = "engine/ui/imm/shaders/runtime_intro.slang";
#endif
    intro_gp.entry_point_vs = "vs_intro";
    intro_gp.entry_point_fs = "fs_intro";
    intro_gp.color_format_count = 1;
    intro_gp.color_formats[0] = static_cast<std::uint8_t>(g::Format::Bgra8Srgb);
    intro_gp.depth_format = 0;
    intro_gp.enable_depth_write = false;
    intro_gp.enable_blend = false;
    out.intro_pipeline = sh::create_graphics(intro_gp);
    if (!out.intro_pipeline.valid()) {
        std::fputs("[ui_overlay] runtime intro background pipeline unavailable\n", stderr);
    }

    return true;
}

void shutdown_runtime_overlay_gpu(RuntimeOverlayGpu& overlay) {
    if (overlay.intro_pipeline.valid()) {
        psynder::shader::destroy_pipeline(overlay.intro_pipeline);
        overlay.intro_pipeline = {};
    }
    if (overlay.pipeline.valid()) {
        psynder::shader::destroy_pipeline(overlay.pipeline);
        overlay.pipeline = {};
    }
    overlay.index_buffer = gpu::Handle<gpu::Buffer>{};
    overlay.vertex_buffer = gpu::Handle<gpu::Buffer>{};
    overlay.font_sampler = gpu::Handle<gpu::Sampler>{};
    overlay.font_texture = gpu::Handle<gpu::Texture>{};
}

bool runtime_overlay_gpu_ready(const RuntimeOverlayGpu& overlay) noexcept {
    return overlay.pipeline.valid() &&
           static_cast<bool>(overlay.vertex_buffer) &&
           static_cast<bool>(overlay.index_buffer) &&
           static_cast<bool>(overlay.font_texture) &&
           static_cast<bool>(overlay.font_sampler);
}

bool begin_runtime_overlay_gpu_frame(gpu::CmdBuffer* cb,
                                     RuntimeOverlayGpu& overlay,
                                     const RuntimeOverlayFrameDesc& desc) {
    if (!cb || !runtime_overlay_gpu_ready(overlay)) {
        return false;
    }

    begin_frame(cb,
                overlay.pipeline,
                overlay.vertex_buffer.get(),
                overlay.index_buffer.get(),
                desc.viewport_width,
                desc.viewport_height);
    detail::batch().shader_time_seconds = desc.time_seconds;
    detail::batch().shader_effect_strength = desc.effect_strength;
    detail::batch().texture = overlay.font_texture.get();
    detail::batch().sampler = overlay.font_sampler.get();
    detail::batch().white_u = 0.5f / static_cast<f32>(kFontAtlasW);
    detail::batch().white_v = 0.5f / static_cast<f32>(kFontAtlasH);
    set_input(desc.mouse_pos, desc.mouse_down);
    auto& canvas = canvas_state();
    canvas.mouse_pos = desc.mouse_pos;
    canvas.mouse_down = desc.mouse_down;
    canvas.mouse_pressed = desc.mouse_pressed;
    canvas.mouse_released = desc.mouse_released;
    if (!desc.mouse_down && !desc.mouse_released) {
        canvas.active_id = 0;
    }
    return true;
}

void end_runtime_overlay_gpu_frame() {
    auto& canvas = canvas_state();
    canvas.mouse_pressed = false;
    canvas.mouse_released = false;
    end_frame();
}

RuntimeOverlayFontMetrics runtime_overlay_font_metrics(float scale) noexcept {
    const ScaledFontMetrics m = font_metrics_scaled(scale);
    return {m.ascent, m.descent, m.body_height, m.line_height};
}

float runtime_overlay_text_width(std::string_view text, float scale) noexcept {
    return text_width_scaled(text, scale);
}

void runtime_overlay_filled_rect(math::Vec2 origin, math::Vec2 size, u32 rgba) {
    filled_rect(origin, size, rgba);
}

void runtime_overlay_rect_outline(math::Vec2 origin, math::Vec2 size, u32 rgba) {
    rect_outline(origin, size, rgba);
}

void runtime_overlay_text(math::Vec2 origin,
                          std::string_view text,
                          u32 rgba,
                          float scale) {
    draw_text_scaled(origin, text, rgba, scale);
}

RuntimeOverlayButtonResult runtime_overlay_button(
    math::Vec2 origin,
    math::Vec2 size,
    std::string_view label,
    const RuntimeOverlayButtonStyle& style) {
    auto& canvas = canvas_state();
    const u64 id = hash_button_id(origin, size, label);
    const bool hovered = point_in_rect(canvas.mouse_pos, origin, size);
    if (hovered && canvas.mouse_pressed) {
        canvas.active_id = id;
    }
    const bool pressed = canvas.mouse_down && canvas.active_id == id;
    const bool clicked = hovered && canvas.mouse_released && canvas.active_id == id;
    if (canvas.mouse_released && canvas.active_id == id) {
        canvas.active_id = 0;
    }

    const u32 bg = pressed ? style.bg_pressed : (hovered ? style.bg_hover : style.bg);
    runtime_overlay_filled_rect(origin, size, bg);
    runtime_overlay_rect_outline(origin, size, style.border);
    const float text_w = runtime_overlay_text_width(label, style.text_scale);
    const RuntimeOverlayFontMetrics metrics =
        runtime_overlay_font_metrics(style.text_scale);
    runtime_overlay_text({origin.x + (size.x - text_w) * 0.5f,
                          origin.y + (size.y - metrics.body_height) * 0.5f},
                         label,
                         style.text,
                         style.text_scale);
    return {hovered, pressed, clicked};
}

void draw_runtime_intro_background_gpu(gpu::CmdBuffer* cb,
                                       const RuntimeOverlayGpu& overlay,
                                       const RuntimeIntroBackgroundDesc& desc) {
    if (!cb || !overlay.intro_pipeline.valid()) {
        return;
    }

    struct PushConstants {
        f32 viewport_width;
        f32 viewport_height;
        f32 time_seconds;
        f32 intensity;
    };
    const PushConstants pc{
        desc.viewport_width > 0.0f ? desc.viewport_width : 1.0f,
        desc.viewport_height > 0.0f ? desc.viewport_height : 1.0f,
        desc.time_seconds,
        std::clamp(desc.intensity, 0.0f, 1.0f),
    };

    psynder::gpu::bind_pipeline(cb, overlay.intro_pipeline);
    psynder::gpu::push_constants(cb,
                                 &pc,
                                 static_cast<u32>(sizeof(pc)),
                                 psynder::gpu::ShaderStage::AllGfx);
    psynder::gpu::draw(cb, 3, 1, 0, 0);
}

bool init_runtime_console_gpu(gpu::Device* device, RuntimeConsoleGpu& out) {
    return init_runtime_overlay_gpu(device, out.overlay);
}

void shutdown_runtime_console_gpu(RuntimeConsoleGpu& overlay) {
    shutdown_runtime_overlay_gpu(overlay.overlay);
    overlay.opened_at_seconds = 0.0;
    overlay.was_open = false;
}

bool runtime_console_gpu_wants_draw(RuntimeConsoleGpu& overlay) {
    if (!runtime_console_gpu_ready(overlay)) {
        return false;
    }
    return sync_open_animation(overlay) > 0.0f;
}

bool runtime_console_gpu_ready(const RuntimeConsoleGpu& overlay) noexcept {
    return runtime_overlay_gpu_ready(overlay.overlay);
}

void draw_runtime_console_gpu(gpu::CmdBuffer* cb,
                              RuntimeConsoleGpu& overlay,
                              float viewport_width,
                              float viewport_height) {
    (void)draw_runtime_overlays_gpu(cb, overlay, viewport_width, viewport_height, nullptr);
}

bool draw_runtime_overlays_gpu(gpu::CmdBuffer* cb,
                               RuntimeConsoleGpu& overlay,
                               float viewport_width,
                               float viewport_height,
                               const RuntimeBootOverlayDesc* boot_overlay,
                               const RuntimeOverlayFrameDesc* frame_desc) {
    const float open_t = sync_open_animation(overlay);
    if (!cb || !runtime_console_gpu_ready(overlay) ||
        (open_t <= 0.0f && !boot_overlay)) {
        return false;
    }

    RuntimeOverlayFrameDesc frame{};
    if (frame_desc) {
        frame = *frame_desc;
    }
    frame.viewport_width = viewport_width;
    frame.viewport_height = viewport_height;
    if (boot_overlay) {
        frame.time_seconds = boot_overlay->time_seconds;
        frame.effect_strength = std::max(frame.effect_strength, 1.0f);
    }
    if (!begin_runtime_overlay_gpu_frame(cb, overlay.overlay, frame)) {
        return false;
    }
    bool boot_action_clicked = false;
    if (boot_overlay) {
        boot_action_clicked = draw_boot_overlay(viewport_width, viewport_height, *boot_overlay);
    }
    if (open_t > 0.0f) {
        draw_console_contents(viewport_width, viewport_height, open_t);
    }
    end_runtime_overlay_gpu_frame();
    return boot_action_clicked;
}

} // namespace psynder::ui::imm
