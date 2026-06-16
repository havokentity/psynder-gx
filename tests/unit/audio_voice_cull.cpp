// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: deterministic voice culling (VoiceCull.h),
// the finite-voice-budget selector that picks the most audible/important
// emitters when more sounds want to play than there are voices.
//
// Verifies the load-bearing properties of engine/audio/VoiceCull.h:
//   (a) voice_score is higher for a nearer same-priority emitter, higher for a
//       higher-priority same-distance emitter, and ~0 at/beyond max_dist;
//   (b) is_audible is the strict d < max quick-reject;
//   (c) select_voices keeps everything (audible) when under budget;
//   (d) over budget it keeps exactly the top-K by score (descending), and an
//       exact score tie breaks to the LOWER id;
//   (e) an inaudible (at/beyond max) candidate is dropped even with spare
//       budget;
//   (f) out_kept is in descending-score order;
//   (g) empty input (and zero budget) => empty output; and
//   (h) determinism: two identical selections are identical.

#include "audio/VoiceCull.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::audio;

// Canonical rolloff band shared by the cases below.
static constexpr f32 kRef = 1.0f;
static constexpr f32 kMax = 100.0f;
static constexpr f32 kRolloff = 1.0f;

TEST_CASE("audio: voice_score ranks nearer and higher-priority emitters higher", "[audio][voicecull]") {
    // Same priority, different distance => the nearer one scores higher.
    const VoiceCandidate near_c{0u, 1.0f, 2.0f};
    const VoiceCandidate far_c{1u, 1.0f, 40.0f};
    const f32 s_near = voice_score(near_c, kRef, kMax, kRolloff);
    const f32 s_far = voice_score(far_c, kRef, kMax, kRolloff);
    INFO("s_near=" << s_near << " s_far=" << s_far);
    REQUIRE(s_near > s_far);
    REQUIRE(s_far > 0.0f);

    // Same distance, different priority => the louder/important one scores higher.
    const VoiceCandidate quiet{2u, 1.0f, 10.0f};
    const VoiceCandidate loud{3u, 4.0f, 10.0f};
    const f32 s_quiet = voice_score(quiet, kRef, kMax, kRolloff);
    const f32 s_loud = voice_score(loud, kRef, kMax, kRolloff);
    INFO("s_quiet=" << s_quiet << " s_loud=" << s_loud);
    REQUIRE(s_loud > s_quiet);

    // At and beyond the max radius => inaudible (~0), regardless of priority.
    const VoiceCandidate at_max{4u, 100.0f, kMax};
    const VoiceCandidate past_max{5u, 100.0f, kMax + 25.0f};
    REQUIRE(voice_score(at_max, kRef, kMax, kRolloff) == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(voice_score(past_max, kRef, kMax, kRolloff) == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("audio: is_audible is the strict less-than-max quick reject", "[audio][voicecull]") {
    REQUIRE(is_audible(VoiceCandidate{0u, 1.0f, 0.0f}, kMax));
    REQUIRE(is_audible(VoiceCandidate{1u, 1.0f, kMax - 0.01f}, kMax));
    REQUIRE_FALSE(is_audible(VoiceCandidate{2u, 1.0f, kMax}, kMax));
    REQUIRE_FALSE(is_audible(VoiceCandidate{3u, 1.0f, kMax + 10.0f}, kMax));
}

TEST_CASE("audio: select_voices keeps every audible candidate when under budget", "[audio][voicecull]") {
    const std::vector<VoiceCandidate> cands{
        {10u, 1.0f, 5.0f},
        {11u, 2.0f, 5.0f},
        {12u, 0.5f, 5.0f},
    };
    std::vector<u32> kept;
    select_voices(cands, /*max_voices=*/8, kRef, kMax, kRolloff, kept);

    // All three are audible and there is room => all kept, descending score.
    // Same distance, so priority orders them: 11 (2.0) > 10 (1.0) > 12 (0.5).
    REQUIRE(kept.size() == 3u);
    REQUIRE(kept[0] == 11u);
    REQUIRE(kept[1] == 10u);
    REQUIRE(kept[2] == 12u);
}

TEST_CASE("audio: select_voices keeps exactly the top-K by score over budget", "[audio][voicecull]") {
    // Construct a clear score ordering (all at the same distance so priority is
    // the sole discriminator): priorities 5,4,3,2,1 => scores strictly ordered.
    const std::vector<VoiceCandidate> cands{
        {100u, 1.0f, 10.0f},  // lowest score
        {101u, 5.0f, 10.0f},  // highest score
        {102u, 3.0f, 10.0f},
        {103u, 4.0f, 10.0f},
        {104u, 2.0f, 10.0f},
    };
    std::vector<u32> kept;
    select_voices(cands, /*max_voices=*/3, kRef, kMax, kRolloff, kept);

    // Top-3 by score, descending: pri 5 (101), 4 (103), 3 (102).
    REQUIRE(kept.size() == 3u);
    REQUIRE(kept[0] == 101u);
    REQUIRE(kept[1] == 103u);
    REQUIRE(kept[2] == 102u);
}

TEST_CASE("audio: select_voices drops an inaudible candidate even with spare budget", "[audio][voicecull]") {
    const std::vector<VoiceCandidate> cands{
        {20u, 1.0f, 5.0f},            // audible
        {21u, 100.0f, kMax + 1.0f},   // beyond max => inaudible (score 0) despite huge priority
        {22u, 1.0f, 8.0f},            // audible
    };
    std::vector<u32> kept;
    // Budget is larger than the candidate count: there is room for all three,
    // but the inaudible one must still be dropped.
    select_voices(cands, /*max_voices=*/10, kRef, kMax, kRolloff, kept);

    REQUIRE(kept.size() == 2u);
    // 20 (nearer, 5 m) scores above 22 (8 m) at equal priority.
    REQUIRE(kept[0] == 20u);
    REQUIRE(kept[1] == 22u);
    for (const u32 id : kept) {
        REQUIRE(id != 21u);
    }
}

TEST_CASE("audio: select_voices breaks an exact score tie to the lower id", "[audio][voicecull]") {
    // Two candidates with identical priority AND distance => identical score.
    // With a budget of 1, the LOWER id must win.
    const std::vector<VoiceCandidate> tie{
        {77u, 2.0f, 6.0f},
        {42u, 2.0f, 6.0f},
    };
    std::vector<u32> kept;
    select_voices(tie, /*max_voices=*/1, kRef, kMax, kRolloff, kept);
    REQUIRE(kept.size() == 1u);
    REQUIRE(kept[0] == 42u);

    // With room for both, the tie still lists the lower id FIRST (descending
    // score, ascending id within the tie).
    select_voices(tie, /*max_voices=*/2, kRef, kMax, kRolloff, kept);
    REQUIRE(kept.size() == 2u);
    REQUIRE(kept[0] == 42u);
    REQUIRE(kept[1] == 77u);
}

TEST_CASE("audio: select_voices output is in descending score order", "[audio][voicecull]") {
    // A mix of priorities and distances; verify the kept ids come out in
    // non-increasing score order by re-scoring them.
    const std::vector<VoiceCandidate> cands{
        {1u, 1.0f, 50.0f},
        {2u, 3.0f, 3.0f},
        {3u, 2.0f, 20.0f},
        {4u, 5.0f, 80.0f},
        {5u, 1.5f, 1.0f},
    };
    std::vector<u32> kept;
    select_voices(cands, /*max_voices=*/4, kRef, kMax, kRolloff, kept);
    REQUIRE(kept.size() == 4u);

    // Rebuild a quick id->score lookup and assert monotonic non-increasing.
    auto score_of = [&](u32 id) -> f32 {
        for (const VoiceCandidate& c : cands) {
            if (c.id == id) return voice_score(c, kRef, kMax, kRolloff);
        }
        return -1.0f;
    };
    for (std::size_t i = 1; i < kept.size(); ++i) {
        const f32 prev = score_of(kept[i - 1]);
        const f32 cur = score_of(kept[i]);
        INFO("kept[" << (i - 1) << "]=" << kept[i - 1] << " score=" << prev
             << " | kept[" << i << "]=" << kept[i] << " score=" << cur);
        REQUIRE(prev >= cur);
    }
}

TEST_CASE("audio: select_voices on empty input and zero budget yields empty output", "[audio][voicecull]") {
    std::vector<u32> kept{99u, 98u};  // pre-populated: must be cleared.

    // Empty candidate span.
    const std::vector<VoiceCandidate> empty;
    select_voices(empty, /*max_voices=*/4, kRef, kMax, kRolloff, kept);
    REQUIRE(kept.empty());

    // Non-empty candidates but zero budget.
    const std::vector<VoiceCandidate> some{{1u, 1.0f, 5.0f}, {2u, 1.0f, 5.0f}};
    kept.assign({7u});
    select_voices(some, /*max_voices=*/0, kRef, kMax, kRolloff, kept);
    REQUIRE(kept.empty());
}

TEST_CASE("audio: voice culling is deterministic across identical selections", "[audio][voicecull]") {
    const std::vector<VoiceCandidate> cands{
        {5u, 2.0f, 12.0f},
        {3u, 2.0f, 12.0f},   // ties with id 5 in score (tie-break to 3)
        {9u, 4.0f, 30.0f},
        {1u, 1.0f, 2.0f},
        {7u, 3.0f, 55.0f},
    };
    std::vector<u32> a, b;
    select_voices(cands, /*max_voices=*/3, kRef, kMax, kRolloff, a);
    select_voices(cands, /*max_voices=*/3, kRef, kMax, kRolloff, b);
    REQUIRE(a == b);

    // scoring is also bit-stable for identical inputs.
    REQUIRE(voice_score(cands[0], kRef, kMax, kRolloff)
            == voice_score(cands[0], kRef, kMax, kRolloff));
}
