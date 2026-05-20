// SPDX-License-Identifier: MIT
// Psynder — Audio Engine implementation (Lane 12). Owns the 32-channel
// software mixer, voice pool, HRTF / FDN / FFT-convolution reverb. Public
// header `engine/audio/Audio.h` is FROZEN; this TU implements it.
//
// Mixer pull contract (DESIGN.md §10.2 + Wave-A rules):
//   * No `virtual` in the hot path (Engine and its inner state are plain
//     structs / functions).
//   * No per-frame `new` / `delete` — all scratch is pre-sized in start().
//   * SIMD-merged voice sum is performed by `detail::simd_mix_into`.
//   * One job per active voice via `jobs::JobSystem::parallel_for`.

#include "Audio.h"

#include "Bus.h"
#include "BusMixer.h"
#include "internal/Backend.h"
#include "internal/BusRouter.h"
#include "internal/MixerCore.h"

#include "core/Log.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace psynder::audio {

namespace {

// One concrete engine instance returned by Engine::Get(). The Engine class
// is non-virtual (DESIGN.md §3.4 / Wave-A mixer hot-path rule). All mutable
// state lives here so Engine itself stays a thin POD facade — the FROZEN
// public header declares no member fields.
struct State {
    std::atomic<bool>           running{false};
    Backend                     chosen_backend = Backend::Auto;
    DeviceDesc                  desc{};

    detail::VoicePool           voices{};
    mutable std::mutex          voices_mu{};

    // listener pose
    math::Vec3                  eye{0, 0, 0};
    math::Vec3                  forward{0, 0, 1};
    math::Vec3                  up{0, 1, 0};

    // wet/dry reverb chains
    detail::FdnReverb           fdn{};
    detail::FftConvReverb       indoor{};
    bool                        reverbs_ready = false;

    // mixer scratch — sized once in start(), reused on every pull.
    std::vector<f32>            scratch_stereo{};                // 2*frames
    std::vector<f32>            per_voice_buffers{};             // kMaxVoices*2*frames
    std::vector<f32>            reverb_send_mono{};              // frames (bus → FDN feed)
};

State& state() {
    static State s;
    return s;
}

void mixer_pull(f32* out_stereo, u32 frames, void* /*user*/) noexcept {
    State& s = state();
    const u32 stereo_floats = 2u * frames;
    if (!s.running.load(std::memory_order_acquire)) {
        std::memset(out_stereo, 0, sizeof(f32) * stereo_floats);
        return;
    }
    // Guard against an oversize pull (backend gave us more frames than
    // start() reserved for). In that case fall through with silence — the
    // platform lane is expected to honour desc.buffer_frames.
    if (frames > s.desc.buffer_frames) {
        std::memset(out_stereo, 0, sizeof(f32) * stereo_floats);
        return;
    }

    std::fill(s.scratch_stereo.begin(),
              s.scratch_stereo.begin() + stereo_floats,
              0.0f);
    std::memset(out_stereo, 0, sizeof(f32) * stereo_floats);

    // Open a new bus-mixer frame. This zeros per-bus accumulators and
    // latches target gains under try_lock (RT-safe). Done unconditionally
    // even when no voices are active so the bus mixer's `last_frames_`
    // book-keeping stays consistent across pulls.
    detail::BusMixer& mixer = detail::bus_mixer();
    mixer.begin_frame(frames);
    std::fill(s.reverb_send_mono.begin(),
              s.reverb_send_mono.begin() + frames,
              0.0f);

    // Snapshot active voices under the voice lock so the worker jobs can run
    // lock-free against an immutable view of the voice array. Also snapshot
    // the per-voice slot index so the bus accumulation pass below can look
    // up its BusId without re-locking the voices mutex.
    detail::Voice snapshot[detail::kMaxVoices];
    u32           snapshot_slot[detail::kMaxVoices];
    u32           snapshot_count = 0;
    {
        std::lock_guard<std::mutex> lk(s.voices_mu);
        for (u32 i = 0; i < detail::kMaxVoices; ++i) {
            const detail::Voice& v = s.voices.at(i);
            if (v.active) {
                snapshot_slot[snapshot_count] = i;
                snapshot[snapshot_count++]    = v;
            }
        }
    }

    if (snapshot_count > 0u) {
        auto& js = jobs::JobSystem::Get();
        // Each voice renders into its own slot of the pre-sized per-voice
        // buffer; zero just the in-use portion (no allocations).
        const usize per_voice_floats = static_cast<usize>(stereo_floats);
        std::fill(s.per_voice_buffers.begin(),
                  s.per_voice_buffers.begin() + static_cast<isize>(snapshot_count * per_voice_floats),
                  0.0f);
        js.parallel_for(0u, snapshot_count, 1u,
            [&](usize begin, usize end) {
                for (usize vi = begin; vi < end; ++vi) {
                    f32* buf = s.per_voice_buffers.data() + vi * per_voice_floats;
                    // 440 Hz placeholder tone — real PCM clip streaming
                    // lands in Wave B. The deliverable here is the mixer
                    // structure, not playback fidelity.
                    detail::render_voice_into_stereo(snapshot[vi], 440.0f,
                                                     s.desc.sample_rate, frames, buf);
                }
            });
        // Route each per-voice buffer into its bus accumulator (lane 23
        // submix graph). This is the integration hook: the previous
        // direct simd_mix_into(scratch_stereo, ...) summing path is
        // replaced by per-bus accumulation, gain-staged downstream in
        // bus_mixer.mix_buses().
        for (u32 vi = 0; vi < snapshot_count; ++vi) {
            const f32* buf = s.per_voice_buffers.data() + static_cast<usize>(vi) * per_voice_floats;
            const BusId b  = detail::slot_bus(snapshot_slot[vi]);
            mixer.accumulate_voice(b, buf, frames);
        }
        // bump cursors under lock so next pull continues smoothly.
        {
            std::lock_guard<std::mutex> lk(s.voices_mu);
            for (u32 i = 0; i < detail::kMaxVoices; ++i) {
                if (s.voices.at(i).active) s.voices.at(i).cursor += frames;
            }
        }
    }

    // Apply per-bus gain ramp + LP filter; sum buses into the master
    // scratch buffer; tee reverb sends into the FDN input buffer. The
    // FDN itself is wired through in Wave B (DESIGN §10.2) — we hold the
    // computed `reverb_send_mono` here so the next-stage reverb pass can
    // consume it without re-deriving per-bus contributions.
    mixer.mix_buses(s.scratch_stereo.data(),
                    s.reverb_send_mono.data(),
                    frames);

    // Master soft-clip + emit. The reverb send is wired through the FDN
    // when an outdoor scene flag is set (lit by sample apps in Wave B);
    // for Wave A we keep the master path dry so the deliverable's mixer
    // bar (per-voice quiet sum doesn't clip) is verifiable directly.
    for (u32 i = 0; i < stereo_floats; ++i) {
        f32 v = s.scratch_stereo[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        out_stereo[i] = v;
    }
}

}  // namespace

// ─── Engine facade ──────────────────────────────────────────────────────
Engine& Engine::Get() {
    static Engine e;
    return e;
}

bool Engine::start(const DeviceDesc& desc) {
    State& s = state();
    if (s.running.load(std::memory_order_acquire)) {
        PSY_LOG_WARN("[audio] Engine::start called twice; ignoring.");
        return true;
    }
    s.desc = desc;
    if (s.desc.channels != 2u) s.desc.channels = 2u;        // mixer is stereo for Wave A
    if (s.desc.sample_rate == 0u) s.desc.sample_rate = 48000u;
    if (s.desc.buffer_frames == 0u) s.desc.buffer_frames = 512u;

    // Pre-size mixer scratch so the hot path never allocates.
    s.scratch_stereo.assign(2u * static_cast<usize>(s.desc.buffer_frames), 0.0f);
    s.per_voice_buffers.assign(
        static_cast<usize>(detail::kMaxVoices) * 2u * static_cast<usize>(s.desc.buffer_frames),
        0.0f);
    s.reverb_send_mono.assign(static_cast<usize>(s.desc.buffer_frames), 0.0f);

    s.voices.clear();
    s.fdn.reset(s.desc.sample_rate,        /*decay s*/   2.5f);
    s.indoor.reset(s.desc.sample_rate,     /*ir s*/      0.12f, /*decay s*/ 1.2f);
    s.reverbs_ready = true;

    // Initialise the global bus router (lane 23). router_reset() clears
    // any leftover voice-slot liveness flags + per-bus gains; init_mixer
    // sizes the BusMixer's accumulators to the negotiated frame size.
    detail::router_reset();
    if (!detail::router_init_mixer(s.desc.sample_rate, s.desc.buffer_frames)) {
        PSY_LOG_WARN("[audio] BusMixer::init failed: sr={} frames={}",
                     s.desc.sample_rate, s.desc.buffer_frames);
    }

    if (!backend_init(s.desc, &mixer_pull, &s, s.chosen_backend)) {
        PSY_LOG_ERROR("[audio] backend_init failed for desc.backend={}",
                      static_cast<int>(s.desc.backend));
        return false;
    }
    s.running.store(true, std::memory_order_release);
    PSY_LOG_INFO("[audio] Engine started — backend={}, sr={}, frames={}",
                 static_cast<int>(s.chosen_backend), s.desc.sample_rate, s.desc.buffer_frames);
    return true;
}

void Engine::stop() {
    State& s = state();
    if (!s.running.exchange(false, std::memory_order_acq_rel)) return;
    backend_shutdown(s.chosen_backend);
    {
        std::lock_guard<std::mutex> lk(s.voices_mu);
        s.voices.clear();
    }
    // Clear the bus router (per-slot liveness shadow + bus targets).
    detail::router_reset();
    PSY_LOG_INFO("[audio] Engine stopped.");
}

VoiceId Engine::play(ClipId clip, math::Vec3 position, f32 volume) {
    State& s = state();
    u32 packed = 0;
    {
        std::lock_guard<std::mutex> lk(s.voices_mu);
        packed = s.voices.acquire(clip.raw, position, volume);
    }
    if (packed != 0u) {
        // Sync the bus-router shadow with the voice pool. The router
        // defaults the new voice to BusId::Sfx; gameplay code calls
        // route_voice_to_bus() to override per-effect.
        const u32 idx = detail::VoicePool::unpack_index(packed);
        const u32 gen = detail::VoicePool::unpack_gen(packed);
        detail::mark_voice_live(idx, gen);
    }
    return VoiceId{ packed };
}

void Engine::stop_voice(VoiceId voice) {
    State& s = state();
    bool released = false;
    {
        std::lock_guard<std::mutex> lk(s.voices_mu);
        released = s.voices.release(voice.raw);
    }
    if (released) {
        const u32 idx = detail::VoicePool::unpack_index(voice.raw);
        detail::mark_voice_dead(idx);
    }
}

void Engine::set_listener(math::Vec3 eye, math::Vec3 forward, math::Vec3 up) {
    State& s = state();
    s.eye = eye; s.forward = forward; s.up = up;
}

u32 Engine::active_voice_count() const {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.voices_mu);
    return s.voices.active_count();
}

}  // namespace psynder::audio
