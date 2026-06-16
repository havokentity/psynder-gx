// SPDX-License-Identifier: MIT
//
// engine/audio/Ducking.h
//
// Lane 14 — sidechain ducking (the standard audio "compressor" envelope that
// auto-lowers one bus when a louder trigger plays). When a high-priority sound
// is active — a radio call, a key gameplay SFX — the music/ambience bus is
// smoothly attenuated DOWN to a floor so the trigger reads clearly; when the
// trigger stops the bus is RELEASED back up to unity. This is the classic
// sidechain compressor envelope: an attack while the sidechain is hot, a
// release while it is quiet.
//
// SCOPE: pure scalar envelope math — no device I/O, no event queue, no bus
// graph. The caller advances the envelope each mix block with the trigger
// state and the block's dt, then multiplies the ducked bus by duck_gain().
// Cosmetic (not the authoritative lockstep tick), allocation-free, noexcept,
// deterministic (pure +,-,*,/ — no trig, RNG, or wall-clock). Gains are linear
// in [0,1]; rates are gain-units per second; dt is seconds. 1 unit = 1 m
// (unused here; stated for lane consistency).

#pragma once

#include "core/Types.h"

namespace psynder::audio {

// Current ducking envelope state: the linear gain to multiply the ducked bus
// by. Lives in [duck_floor, 1]; 1.0 means fully released (no ducking), the
// floor means fully ducked. POD — copy/compare freely.
struct DuckEnvelope {
    f32 gain;  // current linear gain, [floor, 1]
};

// Static configuration of the sidechain. POD.
//   duck_floor    — the ducked gain floor in [0,1] (how far the bus drops while
//                   the trigger is hot; 0 = silence, 1 = no ducking at all).
//   attack_per_s  — rate (gain units / second) the gain moves DOWN toward the
//                   floor while the trigger is active. Larger = ducks faster.
//   release_per_s — rate (gain units / second) the gain moves UP toward unity
//                   while the trigger is inactive. Larger = recovers faster.
struct DuckParams {
    f32 duck_floor;     // [0,1]
    f32 attack_per_s;   // gain units / second, >= 0
    f32 release_per_s;  // gain units / second, >= 0
};

// Initialise an envelope to the un-ducked state (gain = 1.0).
void duck_init(DuckEnvelope& e) noexcept;

// Advance the envelope by one block of `dt_s` seconds.
//
// When `trigger_active` is true the gain moves DOWN toward `duck_floor` by
// attack_per_s * dt_s; otherwise it moves UP toward 1.0 by release_per_s * dt_s.
// The move never overshoots the target (it snaps exactly onto the target rather
// than crossing it) and the result is always clamped to [duck_floor, 1].
//
// Guard: a non-finite or non-positive `dt_s` (NaN/inf/<=0) performs NO move and
// leaves the envelope unchanged, so a stalled or garbage frame cannot corrupt
// the gain. A floor outside [0,1] is clamped so the envelope stays well-formed.
void duck_update(DuckEnvelope& e, const DuckParams& p, bool trigger_active,
                 f32 dt_s) noexcept;

// The linear gain to multiply the ducked bus by this block.
f32 duck_gain(const DuckEnvelope& e) noexcept;

// The steady-state target the envelope is heading toward right now: the
// (clamped) duck_floor when the trigger is active, else 1.0 (unity). Handy for
// callers that want the destination without stepping the envelope.
f32 duck_target(const DuckParams& p, bool trigger_active) noexcept;

}  // namespace psynder::audio
