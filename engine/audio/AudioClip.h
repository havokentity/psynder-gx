// SPDX-License-Identifier: MIT
//
// engine/audio/AudioClip.h
//
// Lane 14 — PCM audio clip + minimal WAV loader.
//
// An AudioClip is a slab of int16 PCM samples (mono or stereo) plus its
// native sample rate and a debug name. The mixer streams from clips; this
// is the replacement for the prior 440 Hz placeholder tone.
//
// WAV support: chunked-RIFF with PCM int16 only (format tag 1). FLAC / Ogg
// / float32 / 24-bit / IMA-ADPCM all deferred — int16 is what every game
// asset pipeline cooks to anyway, and the mixer resamples on the fly.
//
// VFS:
//   load_wav_file()   takes a real filesystem path. Useful for the offline
//                     RIR baker tool and for unit tests with a fixture path.
//   load_wav_memory() takes a buffer already loaded by something else
//                     (VFS, embedded resource, network, etc).
//
// Both return true on success. On failure the AudioClip is left empty
// (samples cleared, sample_rate = 0).
//
// Thread-safety: the AudioClip is owned + read-only after load. Mixer
// reads `samples`, `sample_rate`, `channels` from any thread.

#pragma once

#include "core/Types.h"
#include "Audio.h"           // for ClipId

#include <cstddef>
#include <string>
#include <vector>

namespace psynder::audio {

// One playable buffer of int16 PCM. Sample interleave for stereo:
//   samples[2*frame + 0] = left
//   samples[2*frame + 1] = right
// Mono clips: samples[frame] = mono sample.
struct AudioClip {
    std::vector<i16> samples;        // int16 PCM, interleaved if stereo
    u32              sample_rate = 0;
    u8               channels    = 0;   // 1 = mono, 2 = stereo
    std::string      name;

    // Convenience: number of frames (samples / channels). One frame is one
    // time-tick of audio regardless of channel count.
    u32 frame_count() const noexcept {
        if (channels == 0u) return 0u;
        return static_cast<u32>(samples.size()) / static_cast<u32>(channels);
    }

    bool empty() const noexcept { return samples.empty() || sample_rate == 0u; }
    void reset() noexcept {
        samples.clear();
        sample_rate = 0;
        channels    = 0;
        name.clear();
    }
};

// ─── WAV loader ─────────────────────────────────────────────────────────
//
// Both entry points populate `out_clip` on success and leave it empty on
// failure. The loader is permissive about chunk order (the FACT chunk and
// padding bytes are skipped), and strict about PCM-int16 only.

// Load from a raw filesystem path. `path` is a UTF-8 OS path
// (not a VFS virtual path — `load_wav_vfs` would wrap Vfs::read but the
// VFS lives in lane 05; lane 14 does not depend on it for the test paths).
bool load_wav_file(const char* path, AudioClip& out_clip) noexcept;

// Load from an in-memory buffer (already-read .wav bytes).
bool load_wav_memory(const void* data, std::size_t bytes,
                     AudioClip& out_clip) noexcept;

// ─── Clip registry ──────────────────────────────────────────────────────
//
// The Engine plays clips by ClipId. To get a ClipId you register a loaded
// AudioClip with the engine. The registry owns the clip storage (the
// caller's AudioClip is moved or copied in) and returns an opaque id
// usable in Engine::play().
//
// These functions are intentionally OUTSIDE Audio.h to keep that frozen
// public surface stable; they live in this header as part of lane 14's
// non-frozen extension surface. Other lanes can include AudioClip.h to
// register clips without recompiling Audio.h consumers.
//
// Thread-safety: the registry is global and serialised by an internal
// mutex. Register from the main/game thread (typical asset-load path).
// Engine::play() is safe to call from any thread.

// Register a clip. Takes ownership of the clip's sample vector (the
// caller's AudioClip is left empty after this returns success). Returns
// kInvalidClipId on failure (registry full).
ClipId register_clip(AudioClip&& clip) noexcept;

// Look up a clip by id. Returns nullptr if the id is invalid or stale.
// Internal-ish helper exposed so tests can verify registration.
const AudioClip* clip_by_id(ClipId id) noexcept;

// Drop all registered clips. Convenient between tests and on shutdown.
// Must NOT be called while voices are active — the mixer holds no
// reference-counted handle to the clip, so an in-flight render would
// dangle. The engine's Engine::stop() ensures no voices are live before
// calling this.
void clear_clip_registry() noexcept;

// Sentinel for "no clip registered". ClipId{0} is reserved; ClipId{1..N}
// are valid registry slots.
inline constexpr ClipId kInvalidClipId{ 0u };

}  // namespace psynder::audio
