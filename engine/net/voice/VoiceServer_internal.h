// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/net/voice/VoiceServer_internal.h
//
// Lane 19 — Server-side voice mixer internals (non-public).
//
// PublicVoice.h is the frozen client-facing contract. Server-only
// lifecycle (init_server / shutdown_server) lives here so it is not
// part of the client ABI.
//
// The dedicated server binary calls init_server(max_listeners) once
// at startup, then calls mix_for_listener() each tick (declared in
// PublicVoice.h and visible to both sides).

#pragma once

#include <cstdint>

namespace psynder::net::voice {

// Allocate one OpusEncoder per listener slot + a lazily-grown decoder
// map keyed by speaker_id.  Must be called before mix_for_listener().
void init_server(std::uint32_t max_listeners);

// Release all encoder / decoder resources allocated by init_server().
void shutdown_server();

} // namespace psynder::net::voice
