// SPDX-License-Identifier: MIT
// Psynder — allocator heatmap impl. See Heatmap.h.

#include "Heatmap.h"

#include "../Profiler.h"  // prof::current_frame + Tracy macros (PSY_ZONE_PLOT)

#include <iterator>

#include <fmt/format.h>

namespace psynder::mem {

const char* tag_name(Tag tag) noexcept {
    switch (tag) {
        case Tag::Render:    return "Render";
        case Tag::Physics:   return "Physics";
        case Tag::Audio:     return "Audio";
        case Tag::Ecs:       return "Ecs";
        case Tag::Asset:     return "Asset";
        case Tag::Streaming: return "Streaming";
        case Tag::Scripts:   return "Scripts";
        case Tag::Tools:     return "Tools";
        case Tag::Misc:      return "Misc";
        case Tag::Count:     return "?";
    }
    return "?";
}

Heatmap& Heatmap::get() noexcept {
    static Heatmap inst;
    return inst;
}

Heatmap::ArenaId Heatmap::register_arena(const char* name,
                                         const LinearArena* arena, Tag tag) {
    if (!name || !arena) return 0u;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : slots_) {
        if (!s.used) {
            s.used  = true;
            s.name  = name;
            s.arena = arena;
            s.tag   = tag;
            s.peak  = arena->used();
            s.id    = next_id_++;
            if (next_id_ == 0u) next_id_ = 1u;  // never recycle the null id
            return s.id;
        }
    }
    return 0u;  // table full
}

void Heatmap::unregister_arena(ArenaId id) {
    if (id == 0u) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : slots_) {
        if (s.used && s.id == id) {
            s = Slot{};
            return;
        }
    }
}

usize Heatmap::arena_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    usize n = 0;
    for (const auto& s : slots_) {
        if (s.used) ++n;
    }
    return n;
}

void Heatmap::sample(HeatmapSnapshot& out) {
    // Per-tag: lock-free reads off the allocator's atomic counters.
    for (usize i = 0; i < kTagCount; ++i) {
        const Tag t         = static_cast<Tag>(i);
        out.tags[i].tag     = t;
        out.tags[i].current = current_usage(t);
        out.tags[i].peak    = peak_usage(t);
    }

    // Per-arena: registry lock, read used()/capacity(), bump the watermark.
    usize n = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : slots_) {
            if (!s.used) continue;
            const usize live = s.arena->used();
            if (live > s.peak) s.peak = live;
            if (n < kMaxArenas) {
                out.arenas[n].name      = s.name;
                out.arenas[n].tag       = s.tag;
                out.arenas[n].used      = live;
                out.arenas[n].capacity  = s.arena->capacity();
                out.arenas[n].peak_used = s.peak;
                ++n;
            }
        }
    }
    out.arena_count = n;
    out.frame       = prof::current_frame();
}

std::string Heatmap::format() {
    HeatmapSnapshot snap;
    sample(snap);

    fmt::memory_buffer buf;
    auto out = std::back_inserter(buf);
    fmt::format_to(out, "allocator heatmap (frame {})\n", snap.frame);
    fmt::format_to(out, "  {:<10} {:>14} {:>14}\n", "tag", "live", "peak");
    for (const auto& t : snap.tags) {
        fmt::format_to(out, "  {:<10} {:>14} {:>14}\n", tag_name(t.tag),
                       t.current, t.peak);
    }
    if (snap.arena_count > 0) {
        fmt::format_to(out, "  {:<20} {:<10} {:>12} {:>12} {:>12} {:>7}\n",
                       "arena", "tag", "used", "capacity", "peak", "fill");
        for (usize i = 0; i < snap.arena_count; ++i) {
            const ArenaSample& a = snap.arenas[i];
            const double fill =
                a.capacity ? static_cast<double>(a.used) * 100.0 /
                                 static_cast<double>(a.capacity)
                           : 0.0;
            fmt::format_to(out,
                           "  {:<20} {:<10} {:>12} {:>12} {:>12} {:>6.1f}%\n",
                           a.name ? a.name : "?", tag_name(a.tag), a.used,
                           a.capacity, a.peak_used, fill);
        }
    }
    return fmt::to_string(buf);
}

void Heatmap::plot_to_tracy() {
#if defined(PSYNDER_ENABLE_TRACY) && PSYNDER_ENABLE_TRACY
    for (usize i = 0; i < kTagCount; ++i) {
        const Tag t = static_cast<Tag>(i);
        PSY_ZONE_PLOT(tag_name(t), static_cast<double>(current_usage(t)));
    }
#endif
}

void Heatmap::reset_peaks() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : slots_) {
        if (s.used && s.arena) s.peak = s.arena->used();
    }
}

}  // namespace psynder::mem
