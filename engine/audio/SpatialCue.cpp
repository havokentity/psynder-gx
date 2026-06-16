// SPDX-License-Identifier: MIT
//
// engine/audio/SpatialCue.cpp
//
// Lane 14 — listener-relative spatial cues.
//
// The entire SpatialCue API is header-inline (POD in / POD out, allocation-free,
// noexcept) so it can live next to PositionalMix.h on the mixer pull path. This
// translation unit exists only to (a) give the GLOB-based audio lane a concrete
// source to compile, and (b) force the header through the compiler standalone so
// it stays self-contained (no missing include surfaces only at first use site).

#include "audio/SpatialCue.h"

namespace psynder::audio {

// Intentionally empty: see the header for the implementation. A
// static_assert keeps the public POD layout honest as a cheap compile-time
// guard that the struct stays trivial (no hidden allocation/vtable creeping in).
static_assert(sizeof(SpatialResult) == 3 * sizeof(f32),
              "SpatialResult must stay a packed POD of three f32 scalars");

}  // namespace psynder::audio
