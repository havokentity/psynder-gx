// SPDX-License-Identifier: MIT
// Psynder — allocator heatmap. Samples per-tag and per-arena live bytes +
// peak for the allocators in engine/core/alloc/, for the editor overlay
// (lane 16 imm-mode heatmap), the console (`mem_heatmap`), and Tracy plots.
//
// The per-tag numbers come straight off the atomic counters Allocator.cpp
// already maintains (current_usage / peak_usage). The per-arena numbers come
// from LinearArenas the caller registers here; the heatmap stores the raw
// pointer and reads used()/capacity() at sample time, tracking a peak
// watermark across samples (the arena itself forgets its high-water on
// reset()).
//
// This builds on the existing allocator; it does not change it.

#pragma once

#include "Allocator.h"

#include "../Types.h"

#include <array>
#include <mutex>
#include <string>

namespace psynder::mem {

// Number of allocator tags, and the cap on simultaneously-registered arenas.
inline constexpr usize kTagCount  = static_cast<usize>(Tag::Count);
inline constexpr usize kMaxArenas = 64;

// Human-readable tag name ("Render", "Physics", ...); "?" if out of range.
const char* tag_name(Tag tag) noexcept;

// One sampled per-tag cell. `current`/`peak` are live bytes now and the
// high-watermark since process start, read from the allocator's relaxed
// atomic counters (advisory).
struct TagSample {
    Tag   tag     = Tag::Misc;
    usize current = 0;
    usize peak    = 0;
};

// One sampled per-arena cell. `used`/`capacity` come straight off the arena;
// `peak_used` is the watermark the heatmap has observed across its own
// sample() calls.
struct ArenaSample {
    const char* name      = nullptr;
    Tag         tag       = Tag::Misc;
    usize       used      = 0;
    usize       capacity  = 0;
    usize       peak_used = 0;
};

// One full sampling pass: every tag, plus every registered arena.
struct HeatmapSnapshot {
    std::array<TagSample, kTagCount>    tags{};
    std::array<ArenaSample, kMaxArenas> arenas{};
    usize arena_count = 0;
    u64   frame       = 0;  // prof::current_frame() at sample time
};

// Process-global allocator heatmap.
//
// Registration is mutex-guarded (cold path). Sampling reads lock-free atomics
// for tags and takes the registry lock briefly for arenas. Register only
// process-stable, long-lived arenas (frame scratch, per-level pools): the
// heatmap stores the raw pointer, so a registered arena must outlive its
// registration and must not be moved afterwards. Reads of a live arena are
// advisory — a value torn by a concurrent bump is acceptable for a heatmap
// and never gates anything (same posture as the allocator's own racy peak).
class Heatmap {
public:
    using ArenaId = u32;

    static Heatmap& get() noexcept;

    // Register `arena` under `name` (must outlive the registration). Returns a
    // non-zero id, or 0 if either arg is null or the table is full.
    ArenaId register_arena(const char* name, const LinearArena* arena, Tag tag);

    // Drop a previously-registered arena. Zero / unknown ids are ignored.
    void unregister_arena(ArenaId id);

    // Count of currently-registered arenas.
    usize arena_count() const;

    // Fill `out` with one sample of every tag and every registered arena.
    // Updates each registered arena's peak watermark as a side effect.
    void sample(HeatmapSnapshot& out);

    // Sample + render an aligned ASCII table (tags, then arenas). Off the hot
    // path; allocates the result string.
    std::string format();

    // Push per-tag live bytes to Tracy as one named plot per tag. Safe to call
    // every frame; no-op when Tracy is compiled out.
    void plot_to_tracy();

    // Clear the per-arena peak watermarks (reset to each arena's current
    // used()). Tag peaks live in the allocator's counters and are untouched.
    void reset_peaks();

private:
    Heatmap() = default;

    struct Slot {
        const char*        name  = nullptr;
        const LinearArena* arena = nullptr;
        Tag                tag   = Tag::Misc;
        usize              peak  = 0;
        ArenaId            id    = 0;
        bool               used  = false;
    };

    mutable std::mutex           mu_;
    std::array<Slot, kMaxArenas> slots_{};
    ArenaId                      next_id_ = 1;
};

}  // namespace psynder::mem
