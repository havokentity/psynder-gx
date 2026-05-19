// SPDX-License-Identifier: MIT
//
// engine/audio/AudioClip.cpp
//
// Lane 14 — Minimal RIFF/WAVE PCM int16 loader.
//
// Format reference (Microsoft RIFF/WAVE):
//   "RIFF" <size:u32> "WAVE"
//     "fmt " <size:u32> <fmt chunk> ...
//     "data" <size:u32> <pcm bytes>
//     other chunks (LIST, FACT, etc.) skipped.
//
// fmt chunk for PCM (format_tag == 1, ≥16 bytes):
//   u16 format_tag           = 1
//   u16 channels
//   u32 sample_rate
//   u32 avg_bytes_per_sec
//   u16 block_align
//   u16 bits_per_sample      // 16 supported
//
// We deliberately reject anything that's not 16-bit PCM. WAVE_FORMAT_EXTENSIBLE
// (0xFFFE) is also accepted iff its SubFormat GUID indicates int16 PCM.
//
// All integer reads are explicitly little-endian (WAV is LE on every host).

#include "AudioClip.h"
#include "core/Log.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace psynder::audio {

namespace {

// ─── LE byte readers ─────────────────────────────────────────────────────
inline std::uint16_t read_u16_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        (static_cast<std::uint16_t>(p[1]) << 8));
}
inline std::uint32_t read_u32_le(const std::uint8_t* p) noexcept {
    return  static_cast<std::uint32_t>(p[0])        |
           (static_cast<std::uint32_t>(p[1]) <<  8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline bool fourcc_eq(const std::uint8_t* p, const char* tag) noexcept {
    return p[0] == static_cast<std::uint8_t>(tag[0]) &&
           p[1] == static_cast<std::uint8_t>(tag[1]) &&
           p[2] == static_cast<std::uint8_t>(tag[2]) &&
           p[3] == static_cast<std::uint8_t>(tag[3]);
}

}  // namespace

bool load_wav_memory(const void* data, std::size_t bytes,
                     AudioClip& out_clip) noexcept {
    out_clip.reset();
    if (!data || bytes < 44u) {
        PSY_LOG_WARN("[audio] load_wav_memory: buffer too small ({} bytes)", bytes);
        return false;
    }
    const auto* base = static_cast<const std::uint8_t*>(data);

    // RIFF header.
    if (!fourcc_eq(base + 0, "RIFF") || !fourcc_eq(base + 8, "WAVE")) {
        PSY_LOG_WARN("[audio] load_wav_memory: not a RIFF/WAVE file.");
        return false;
    }
    const std::uint32_t riff_size = read_u32_le(base + 4);
    // Cap at the actual buffer size (RIFF size excludes the first 8 bytes).
    const std::size_t   eof = (8u + riff_size <= bytes) ? (8u + riff_size) : bytes;
    std::size_t         pos = 12u;  // past "RIFF<size>WAVE"

    bool have_fmt = false;
    u16  channels = 0;
    u32  sample_rate = 0;
    u16  bits_per_sample = 0;
    u16  format_tag = 0;

    const std::uint8_t* data_chunk = nullptr;
    std::size_t         data_chunk_bytes = 0;

    while (pos + 8u <= eof) {
        const std::uint8_t* ck     = base + pos;
        const std::uint32_t ck_sz  = read_u32_le(ck + 4);
        const std::size_t   payload = pos + 8u;
        if (payload + ck_sz > eof) {
            // Truncated chunk — bail.
            PSY_LOG_WARN("[audio] load_wav_memory: truncated chunk at offset {}.", pos);
            break;
        }

        if (fourcc_eq(ck, "fmt ")) {
            if (ck_sz < 16u) {
                PSY_LOG_WARN("[audio] load_wav_memory: fmt chunk too short ({}).", ck_sz);
                return false;
            }
            const std::uint8_t* f = base + payload;
            format_tag      = read_u16_le(f + 0);
            channels        = read_u16_le(f + 2);
            sample_rate     = read_u32_le(f + 4);
            // f + 8: avg_bytes_per_sec
            // f + 12: block_align
            bits_per_sample = read_u16_le(f + 14);

            // WAVE_FORMAT_EXTENSIBLE: check the SubFormat GUID's first u16.
            // GUID for PCM is 00000001-0000-0010-8000-00aa00389b71.
            if (format_tag == 0xFFFE && ck_sz >= 40u) {
                const std::uint16_t sub_fmt = read_u16_le(f + 24);
                if (sub_fmt != 1u) {
                    PSY_LOG_WARN("[audio] load_wav_memory: WAVE_FORMAT_EXTENSIBLE "
                                 "with non-PCM subformat 0x{:x}.", sub_fmt);
                    return false;
                }
                format_tag = 1;  // treat as plain PCM
            }
            have_fmt = true;
        } else if (fourcc_eq(ck, "data")) {
            data_chunk       = base + payload;
            data_chunk_bytes = ck_sz;
        }
        // Chunks are word-aligned (pad byte if odd).
        pos = payload + ck_sz;
        if (ck_sz & 1u) ++pos;
    }

    if (!have_fmt) {
        PSY_LOG_WARN("[audio] load_wav_memory: no fmt chunk.");
        return false;
    }
    if (format_tag != 1u) {
        PSY_LOG_WARN("[audio] load_wav_memory: unsupported format_tag={}, "
                     "only PCM int16 supported.", format_tag);
        return false;
    }
    if (bits_per_sample != 16u) {
        PSY_LOG_WARN("[audio] load_wav_memory: unsupported bits_per_sample={}, "
                     "only 16-bit PCM supported.", bits_per_sample);
        return false;
    }
    if (channels != 1u && channels != 2u) {
        PSY_LOG_WARN("[audio] load_wav_memory: unsupported channels={}, "
                     "only mono/stereo supported.", channels);
        return false;
    }
    if (sample_rate == 0u) {
        PSY_LOG_WARN("[audio] load_wav_memory: sample_rate=0.");
        return false;
    }
    if (!data_chunk || data_chunk_bytes == 0u) {
        PSY_LOG_WARN("[audio] load_wav_memory: no data chunk.");
        return false;
    }
    if ((data_chunk_bytes & 1u) != 0u) {
        // int16 PCM must be even-byte length; trim the odd byte.
        --data_chunk_bytes;
    }

    const std::size_t n_samples = data_chunk_bytes / sizeof(std::int16_t);
    out_clip.samples.resize(n_samples);
    // Memcpy is portable: host is LE (all our targets), and the source is LE.
    // On a hypothetical BE host this would need byte-swapping, but x86 / arm64
    // are both LE so we keep the fast path.
    std::memcpy(out_clip.samples.data(), data_chunk,
                n_samples * sizeof(std::int16_t));
    out_clip.sample_rate = sample_rate;
    out_clip.channels    = static_cast<u8>(channels);
    return true;
}

bool load_wav_file(const char* path, AudioClip& out_clip) noexcept {
    out_clip.reset();
    if (!path || !*path) return false;

    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        PSY_LOG_WARN("[audio] load_wav_file: cannot open '{}'.", path);
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    const long len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (len <= 0) {
        std::fclose(fp);
        PSY_LOG_WARN("[audio] load_wav_file: empty file '{}'.", path);
        return false;
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(len));
    const std::size_t got = std::fread(buf.data(), 1u, buf.size(), fp);
    std::fclose(fp);
    if (got != buf.size()) {
        PSY_LOG_WARN("[audio] load_wav_file: short read on '{}'.", path);
        return false;
    }
    const bool ok = load_wav_memory(buf.data(), buf.size(), out_clip);
    if (ok) out_clip.name = path;
    return ok;
}

// ─── Clip registry ──────────────────────────────────────────────────────
//
// Fixed-size table of registered clips. Generation counters mirror the
// VoicePool / VoiceMix scheme so a stale ClipId after clear_clip_registry()
// compares unequal.
//
// 256 slots is more than any plausible single-level FPS soundpack needs;
// large asset packs that exceed this number indicate the engine needs a
// streaming loader rather than a flat registry.

namespace {

constexpr u32 kMaxRegisteredClips = 256u;

struct ClipSlot {
    bool      occupied = false;
    u32       gen      = 0u;
    AudioClip clip;
};

std::array<ClipSlot, kMaxRegisteredClips> g_clips{};
std::mutex                                g_clips_mu;

constexpr u32 pack_clip(u32 idx, u32 gen) noexcept {
    return ((idx + 1u) & 0x00FFFFFFu) | ((gen & 0xFFu) << 24);
}
constexpr u32 unpack_clip_idx(u32 packed) noexcept {
    const u32 raw_idx = packed & 0x00FFFFFFu;
    return raw_idx == 0u ? 0xFFFFFFFFu : (raw_idx - 1u);
}
constexpr u32 unpack_clip_gen(u32 packed) noexcept {
    return (packed >> 24) & 0xFFu;
}

}  // namespace

ClipId register_clip(AudioClip&& clip) noexcept {
    std::lock_guard<std::mutex> lk(g_clips_mu);
    for (u32 i = 0; i < kMaxRegisteredClips; ++i) {
        if (!g_clips[i].occupied) {
            g_clips[i].occupied = true;
            g_clips[i].gen = (g_clips[i].gen + 1u) & 0xFFu;
            if (g_clips[i].gen == 0u) g_clips[i].gen = 1u;
            g_clips[i].clip = std::move(clip);
            return ClipId{ pack_clip(i, g_clips[i].gen) };
        }
    }
    PSY_LOG_WARN("[audio] register_clip: registry full ({} slots)", kMaxRegisteredClips);
    return kInvalidClipId;
}

const AudioClip* clip_by_id(ClipId id) noexcept {
    if (id.raw == 0u) return nullptr;
    const u32 idx = unpack_clip_idx(id.raw);
    if (idx >= kMaxRegisteredClips) return nullptr;
    // Reads of `occupied` / `gen` happen under the mutex to pair with
    // register/clear. The mixer hot path also takes this lock briefly —
    // see ClipRegistryLookup in Audio.cpp which snapshots clip pointers
    // before the parallel_for so contention is bounded.
    std::lock_guard<std::mutex> lk(g_clips_mu);
    if (!g_clips[idx].occupied) return nullptr;
    if (g_clips[idx].gen != unpack_clip_gen(id.raw)) return nullptr;
    return &g_clips[idx].clip;
}

void clear_clip_registry() noexcept {
    std::lock_guard<std::mutex> lk(g_clips_mu);
    for (auto& s : g_clips) {
        s.occupied = false;
        s.clip.reset();
    }
}

}  // namespace psynder::audio
