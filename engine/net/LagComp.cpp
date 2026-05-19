// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — LagCompContext + rewind impl. Lane 18 (net).
//
// DESIGN-PSYNDER-GX.md §10.4 (Lag compensation):
//   "Server rewinds world state to the client's view-time when validating
//    hitscans. Standard rewind window: 200 ms."
//
// Storage: ring buffer of `cfg.lag_comp_ticks + 1` slots so we can bracket
// any tick inside the window with one older + one newer snapshot for
// linear interpolation. Snapshots arrive in monotonic tick order; the head
// is the newest.
//
// Interpolation model: byte-wise linear interp is meaningless for the
// underlying physics state (it'd interpolate raw memory). Until lane 17
// settles the WorldState layout we treat the bytes as opaque and snap to
// the OLDER bracket snapshot when the sub-tick fraction is < 0.5, and to
// the NEWER snapshot otherwise. Once lane 17 publishes the concrete type
// (filed as a follow-up Issue per the task spec) this function will pivot
// to a structure-aware lerp of per-entity transforms.

#include "LagComp.h"

#include <algorithm>
#include <cstring>

namespace psynder::net {

namespace {

PSY_FORCEINLINE u32 wrap_index(u32 idx, u32 cap) noexcept {
    return cap == 0 ? 0 : (idx % cap);
}

}  // namespace

// ─── LagCompContext ───────────────────────────────────────────────────────

LagCompContext::LagCompContext(const TickConfig& cfg) noexcept
    : cfg_(cfg) {
    // Capacity = lag_comp_ticks + 1 so we can hold the oldest legal rewind
    // target AND the most recent post-tick snapshot simultaneously.
    capacity_ = cfg.lag_comp_ticks + 1u;
    ring_.resize(capacity_);
}

LagCompContext::~LagCompContext() noexcept = default;

u32 LagCompContext::stored_count() const noexcept { return count_; }

u32 LagCompContext::oldest_tick() const noexcept {
    if (count_ == 0 || capacity_ == 0) return 0;
    const u32 oldest_idx = wrap_index(head_ + capacity_ - count_ + 1u, capacity_);
    return ring_[oldest_idx].tick;
}

u32 LagCompContext::newest_tick() const noexcept {
    if (count_ == 0) return 0;
    return ring_[head_].tick;
}

void LagCompContext::push_internal_(u32 tick, const OpaqueWorldState& state) noexcept {
    if (capacity_ == 0) return;

    // Advance head. If we've filled the ring, the oldest entry is evicted
    // by being overwritten.
    if (count_ == 0) {
        head_ = 0;
        count_ = 1;
    } else {
        head_ = wrap_index(head_ + 1u, capacity_);
        if (count_ < capacity_) ++count_;
    }
    Slot& s = ring_[head_];
    s.tick = tick;
    if (state.data && state.size > 0) {
        s.bytes.assign(static_cast<const u8*>(state.data),
                       static_cast<const u8*>(state.data) + state.size);
    } else {
        s.bytes.clear();
    }
}

bool LagCompContext::bracket_internal_(
    u32 target_tick,
    const u8*& out_older_bytes, u32& out_older_tick,
    const u8*& out_newer_bytes, u32& out_newer_tick,
    usize& out_state_size) const noexcept
{
    if (count_ == 0 || capacity_ == 0) return false;

    // Walk slots from oldest to newest.
    const u32 oldest_idx = wrap_index(head_ + capacity_ - count_ + 1u, capacity_);

    const Slot* older = nullptr;
    const Slot* newer = nullptr;
    for (u32 i = 0; i < count_; ++i) {
        const u32 idx = wrap_index(oldest_idx + i, capacity_);
        const Slot& s = ring_[idx];
        if (s.tick <= target_tick) {
            older = &s;
        } else {
            newer = &s;
            break;
        }
    }

    if (!older) {
        // target is older than every stored entry — caller will clamp.
        const u32 idx = oldest_idx;
        out_older_bytes = ring_[idx].bytes.data();
        out_older_tick  = ring_[idx].tick;
        out_newer_bytes = out_older_bytes;
        out_newer_tick  = out_older_tick;
        out_state_size  = ring_[idx].bytes.size();
        return true;
    }
    if (!newer) {
        newer = older;
    }
    out_older_bytes = older->bytes.data();
    out_older_tick  = older->tick;
    out_newer_bytes = newer->bytes.data();
    out_newer_tick  = newer->tick;
    out_state_size  = older->bytes.size();
    return true;
}

// ─── Free functions ───────────────────────────────────────────────────────

void push_world_snapshot(LagCompContext&         ctx,
                         const OpaqueWorldState& state,
                         u32                     tick) noexcept {
    ctx.push_internal_(tick, state);
}

RewindResult rewind_world_to(LagCompContext&   ctx,
                             OpaqueWorldState& out_state,
                             f64               client_view_time_ms,
                             u32               current_tick,
                             u16               sub_tick_frac_u16) noexcept {
    if (ctx.stored_count() == 0) return RewindResult::Unavailable;
    if (!out_state.data || out_state.size == 0) return RewindResult::Unavailable;

    const TickConfig& cfg = ctx.tick_config();
    if (cfg.frame_ms <= 0.0) return RewindResult::Unavailable;

    // Server timeline anchor: current_tick * frame_ms is "now" in the same
    // base as client_view_time_ms.
    const f64 now_ms = static_cast<f64>(current_tick) * cfg.frame_ms;

    // Clamp client_view_time_ms to [now_ms - kMaxLagCompWindowMs, now_ms].
    f64 desired_ms = client_view_time_ms;
    bool clamped = false;
    const f64 floor_ms = now_ms - static_cast<f64>(TickConfig::kMaxLagCompWindowMs);
    if (desired_ms < floor_ms) {
        desired_ms = floor_ms;
        clamped    = true;
    }
    if (desired_ms > now_ms) {
        desired_ms = now_ms;
        clamped    = true;
    }

    // Target tick + fraction.
    const f64 target_tick_f = desired_ms / cfg.frame_ms;
    u32       target_tick   = static_cast<u32>(target_tick_f);
    // The 16-bit sub-tick fraction lives in [0, 0xFFFF]. Combine it with
    // the integer-tick derivation to produce the final fractional tick
    // index used for the interp weight.
    const f64 subtick_frac = static_cast<f64>(sub_tick_frac_u16)
                             / static_cast<f64>(0xFFFFu);
    f64 frac = (target_tick_f - static_cast<f64>(target_tick)) + subtick_frac;
    while (frac >= 1.0) { ++target_tick; frac -= 1.0; }
    if (frac < 0.0) frac = 0.0;

    // Bracket.
    const u8* older = nullptr;
    const u8* newer = nullptr;
    u32       older_tick = 0;
    u32       newer_tick = 0;
    usize     state_size = 0;
    if (!ctx.bracket_internal_(target_tick,
                               older, older_tick,
                               newer, newer_tick,
                               state_size)) {
        return RewindResult::Unavailable;
    }

    if (state_size == 0 || state_size != out_state.size) {
        return RewindResult::Unavailable;
    }
    if (target_tick < ctx.oldest_tick()) {
        clamped = true;
    }

    // Pick the bracket side based on fractional position. See file header
    // for why we don't byte-lerp here.
    const u32 span = (newer_tick > older_tick) ? (newer_tick - older_tick) : 0u;
    f64 w = 0.0;
    if (span > 0) {
        // Position of target_tick + frac inside [older_tick, newer_tick].
        const f64 pos = static_cast<f64>(target_tick - older_tick) + frac;
        w = pos / static_cast<f64>(span);
        if (w < 0.0) w = 0.0;
        if (w > 1.0) w = 1.0;
    }
    const u8* chosen = (w < 0.5) ? older : newer;
    std::memcpy(out_state.data, chosen, state_size);

    return clamped ? RewindResult::Clamped : RewindResult::Ok;
}

}  // namespace psynder::net
