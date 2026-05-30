// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/ScreenFade.cpp — full-screen fade envelope (to/from black or
// white, with a hold). See ScreenFade.h for the convention + determinism
// contract.

#include "camera/ScreenFade.h"

#include <algorithm>  // std::clamp
#include <cmath>      // std::isfinite

namespace psynder::camera {

namespace {

// Clamp an opacity into [0, peak], guarding a degenerate (non-finite or
// negative) peak by collapsing it to 0 so a bad FadeParams never produces a
// NaN / out-of-range alpha.
f32 clamp_alpha(f32 a, f32 peak) noexcept {
    f32 hi = peak;
    if (!(std::isfinite(hi)) || hi < 0.0f) hi = 0.0f;
    return std::clamp(a, 0.0f, hi);
}

}  // namespace

void fade_init(FadeState& s) noexcept {
    s.alpha   = 0.0f;
    s.timer_s = 0.0f;
    s.phase   = kFadePhaseIdle;
}

void fade_start(FadeState& s, const FadeParams& p) noexcept {
    // Always re-arm from the beginning: clear, timer 0.
    s.alpha   = 0.0f;
    s.timer_s = 0.0f;

    // A degenerate (non-finite or non-positive) fade-in is INSTANT: jump to the
    // (clamped) peak and enter HOLD straight away — no visible ramp-in.
    if (std::isfinite(p.fade_in_s) && p.fade_in_s > 0.0f) {
        s.phase = kFadePhaseIn;
    } else {
        s.alpha = clamp_alpha(p.peak_alpha, p.peak_alpha);
        s.phase = kFadePhaseHold;
    }
}

void fade_update(FadeState& s, const FadeParams& p, f32 dt_s) noexcept {
    if (s.phase == kFadePhaseIdle) return;  // cleared — nothing to advance

    // A non-finite or non-positive dt produces no advance (no move, no NaN), so
    // a stalled / bogus frame clock leaves the envelope untouched.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) return;

    const f32 peak = clamp_alpha(p.peak_alpha, p.peak_alpha);

    // The remaining budget of THIS dt to spend; leftover is carried across each
    // phase boundary so a large step spills its remainder into the next phase.
    f32 remaining = dt_s;

    // Walk forward through the phases consuming `remaining`. Each phase has a
    // span; if the in-phase timer plus the budget reaches the span, the phase
    // completes, the budget is reduced by exactly the time it took, and the loop
    // re-evaluates the (new) phase. Otherwise the budget is fully absorbed.
    while (remaining > 0.0f && s.phase != kFadePhaseIdle) {
        if (s.phase == kFadePhaseIn) {
            const f32 span = p.fade_in_s;
            // A degenerate span (shouldn't happen — fade_start routes around it,
            // but belt-and-braces) completes instantly into HOLD at peak.
            if (!(std::isfinite(span)) || span <= 0.0f) {
                s.alpha   = peak;
                s.timer_s = 0.0f;
                s.phase   = kFadePhaseHold;
                continue;
            }
            const f32 left = span - s.timer_s;   // time left in this phase
            if (remaining < left) {
                s.timer_s += remaining;
                s.alpha = clamp_alpha(peak * (s.timer_s / span), peak);
                remaining = 0.0f;
            } else {
                // Consume to the boundary; carry the rest into HOLD.
                remaining -= left;
                s.alpha   = peak;
                s.timer_s = 0.0f;
                s.phase   = kFadePhaseHold;
            }
        } else if (s.phase == kFadePhaseHold) {
            const f32 span = p.hold_s;
            // Pinned at peak through the whole hold.
            s.alpha = peak;
            if (!(std::isfinite(span)) || span <= 0.0f) {
                // No hold — fall straight through to fade-out, carrying all dt.
                s.timer_s = 0.0f;
                s.phase   = kFadePhaseOut;
                continue;
            }
            const f32 left = span - s.timer_s;
            if (remaining < left) {
                s.timer_s += remaining;
                remaining = 0.0f;
            } else {
                remaining -= left;
                s.timer_s = 0.0f;
                s.phase   = kFadePhaseOut;
            }
        } else {  // kFadePhaseOut
            const f32 span = p.fade_out_s;
            // A degenerate span completes instantly to clear / idle.
            if (!(std::isfinite(span)) || span <= 0.0f) {
                s.alpha   = 0.0f;
                s.timer_s = 0.0f;
                s.phase   = kFadePhaseIdle;
                continue;
            }
            const f32 left = span - s.timer_s;
            if (remaining < left) {
                s.timer_s += remaining;
                // Ramp peak -> 0 across the phase.
                s.alpha = clamp_alpha(peak * (1.0f - (s.timer_s / span)), peak);
                remaining = 0.0f;
            } else {
                // Consume to the boundary; the envelope is done and goes idle.
                remaining -= left;
                s.alpha   = 0.0f;
                s.timer_s = 0.0f;
                s.phase   = kFadePhaseIdle;
            }
        }
    }
}

f32 fade_alpha(const FadeState& s) noexcept {
    return s.alpha;
}

bool fade_active(const FadeState& s) noexcept {
    return s.phase != kFadePhaseIdle;
}

}  // namespace psynder::camera
