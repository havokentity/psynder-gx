// SPDX-License-Identifier: MIT
//
// engine/audio/ReverbZone.h
//
// Lane 14 — acoustic-space (reverb zone) parameter model. A "zone" is a region
// of the map with a single reverb character: a tiny closet is nearly dry with a
// short tail, a vast cathedral is wet with a long tail. This header turns a
// space's *size* into a reverb send level (wet) and a reverb tail length
// (decay), and crossfades those parameters when the listener walks from one
// zone into another so the acoustic changes smoothly instead of popping.
//
// SCOPE: pure scalar math — no FDN/convolver, no device I/O, no zone geometry
// queries. The mixer/reverb bus consumes `wet` (send 0..1) and `decay_s` (tail
// seconds); the caller supplies room volumes and the blend factor `t` (e.g.
// from the listener's normalised distance across a zone boundary). Cosmetic
// (not the authoritative lockstep tick), so f32 is fine; allocation-free,
// noexcept, deterministic (pure +,-,*,/ — no trig/RNG/wall-clock).
//
// Units: room_volume_m3 is cubic metres (1 world unit = 1 metre); wet and dry
// gains are linear [0,1]; decay_s is seconds (>= 0).

#pragma once

#include "core/Types.h"

namespace psynder::audio {

// Reference room volume for the saturating size->reverb map (cubic metres).
// At a room of exactly this volume the size parameter t = vol/(vol+ref) = 0.5,
// i.e. the space is half-way to its maximum wet/decay. 1000 m3 is roughly a
// large hall (a ~10x10x10 m room), a sensible "half-wet" pivot between a small
// room and a cathedral.
inline constexpr f32 kReverbReferenceVolumeM3 = 1000.0f;

// Reverb parameters for one acoustic zone.
//   wet     — reverb send level, linear [0,1] (0 = dry/direct only, 1 = soaked).
//   decay_s — reverb tail length in seconds (>= 0).
struct ReverbZoneParams {
    f32 wet;
    f32 decay_s;
};

// Derive reverb parameters from a space's size using a saturating map, so the
// outputs stay in range for any non-negative volume:
//   t       = room_volume / (room_volume + kReverbReferenceVolumeM3)  in [0,1)
//   wet     = max_wet     * t   (clamped to [0,1])
//   decay_s = max_decay_s * t   (clamped to >= 0)
// A bigger room => larger t => more wet + longer decay; a tiny/zero room =>
// t ~ 0 => nearly dry with a near-zero tail. Because t saturates toward 1, even
// an enormous volume can never push wet past max_wet (and thus never past 1
// once clamped) or decay past max_decay_s. A negative volume is treated as 0.
ReverbZoneParams reverb_from_size(f32 room_volume_m3, f32 max_wet,
                                  f32 max_decay_s) noexcept;

// Linear interpolation of a single reverb scalar between two zones, with the
// blend factor clamped to [0,1]: t<=0 returns a, t>=1 returns b, and t in
// between mixes linearly. Used to crossfade any reverb quantity (wet, decay,
// a bus gain) across a zone boundary.
f32 reverb_blend(f32 a, f32 b, f32 t) noexcept;

// Blend two zones' parameters by `t` (clamped [0,1]) field-by-field via
// reverb_blend: the result's wet and decay are each the linear interpolation
// of the two zones' values. At t=0 it equals `a`, at t=1 it equals `b`, and at
// t=0.5 each field is the average of the two — the smooth boundary crossfade.
ReverbZoneParams reverb_lerp(const ReverbZoneParams& a,
                             const ReverbZoneParams& b, f32 t) noexcept;

// The dry (direct-path) gain that pairs with a zone's wet send: 1 - wet,
// clamped to [0,1]. This is the simple complementary wet/dry split — as the
// space gets wetter the direct signal recedes by the same amount — so a fully
// wet zone (wet=1) is all reverb (dry=0) and a fully dry zone (wet=0) is all
// direct (dry=1). It is not equal-power; it is the cheap linear complement the
// reverb bus mixes against.
f32 reverb_dry_gain(const ReverbZoneParams& p) noexcept;

}  // namespace psynder::audio
