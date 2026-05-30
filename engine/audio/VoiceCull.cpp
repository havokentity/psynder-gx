// SPDX-License-Identifier: MIT
//
// engine/audio/VoiceCull.cpp
//
// Lane 14 — deterministic voice culling.
//
// voice_score() / is_audible() are header-inline (see VoiceCull.h). This TU
// implements select_voices(): score every audible candidate once into a small
// {score,id} scratch, then partial-select the top-K by score with a
// deterministic lowest-id tie-break. The input span is never mutated (the
// scratch holds copied score+id pairs, not the candidates), and the scratch is
// an amortised thread_local buffer to keep per-call allocation negligible.

#include "audio/VoiceCull.h"

#include <algorithm>

namespace psynder::audio {

namespace {

// A pre-scored entry: the candidate's audibility score paired with its id.
// Sorting these (rather than re-scoring during comparison) keeps voice_score
// evaluated exactly once per candidate — important since it is the only
// non-trivial arithmetic here and we want a stable, reproducible ordering.
struct Scored {
    f32 score;
    u32 id;
};

}  // namespace

void select_voices(std::span<const VoiceCandidate> candidates,
                   usize max_voices,
                   f32 ref_dist_m,
                   f32 max_dist_m,
                   f32 rolloff,
                   std::vector<u32>& out_kept) noexcept {
    out_kept.clear();

    const usize n = candidates.size();
    if (n == 0 || max_voices == 0) {
        return;
    }

    // Score every candidate once into an amortised thread_local scratch (cleared,
    // not freed, each call, so steady-state selection allocates nothing). It is
    // thread_local so concurrent mixer threads never race on it.
    thread_local std::vector<Scored> scored;
    scored.clear();
    scored.reserve(n);
    for (usize i = 0; i < n; ++i) {
        const VoiceCandidate& c = candidates[i];
        const f32 s = voice_score(c, ref_dist_m, max_dist_m, rolloff);
        // Inaudible / non-positive score => never a candidate for a voice.
        if (s > 0.0f) {
            scored.push_back(Scored{s, c.id});
        }
    }

    if (scored.empty()) {
        return;
    }

    // Deterministic order: higher score first; equal scores break to the LOWER
    // id (so a tie lists the smaller id first AND keeps the smaller id when the
    // budget cuts through the tie). A total, stable-by-construction comparator.
    const auto better = [](const Scored& a, const Scored& b) noexcept {
        if (a.score != b.score) {
            return a.score > b.score;  // higher score is "better" (earlier)
        }
        return a.id < b.id;            // tie => lower id first
    };

    const usize keep = scored.size() < max_voices ? scored.size() : max_voices;

    // Partial selection is enough: get the top `keep` into the front in order,
    // leaving the tail unsorted. std::partial_sort is deterministic given the
    // total comparator above.
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(keep),
                      scored.end(), better);

    out_kept.reserve(keep);
    for (usize i = 0; i < keep; ++i) {
        out_kept.push_back(scored[i].id);
    }
}

}  // namespace psynder::audio
