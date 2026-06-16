// SPDX-License-Identifier: MIT
//
// engine/audio/Footsteps.cpp
//
// Lane 14 — distance-driven footstep cadence timing.
//
// The cadence math is fully header-inline in Footsteps.h (POD in / POD out,
// `noexcept`, allocation-free) so it can be called from the hot movement /
// mixer paths without a function-call barrier. This translation unit exists so
// the audio lane GLOB picks up a Footsteps compilation unit and so the
// header's POD / triviality contract is enforced at build time, in one place,
// rather than relying on every caller to notice a regression.

#include "audio/Footsteps.h"

#include <type_traits>

namespace psynder::audio {

// FootstepState must stay a trivial POD: it is memcpy-able into save/replay
// blobs and network snapshots, and trivially default-constructible so an array
// of them can be value-initialised cheaply. Guard the contract here.
static_assert(std::is_trivial_v<FootstepState>,
              "FootstepState must remain a trivial POD (memcpy/replay safe)");
static_assert(std::is_trivially_copyable_v<FootstepState>,
              "FootstepState must remain trivially copyable (snapshot safe)");
static_assert(std::is_standard_layout_v<FootstepState>,
              "FootstepState must remain standard-layout (stable ABI)");

}  // namespace psynder::audio
