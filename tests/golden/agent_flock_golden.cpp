// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/golden/agent_flock_golden.cpp — golden determinism gate (ARCH-review A11).
//
// The lockstep-critical sim (128-tick rollback netcode, ADR-019 / DESIGN §14)
// MUST be bit-reproducible. This is the AUTHORITATIVE golden: the macOS / arm64
// build is the reference platform (the determinism CI matrix runs Linux/Windows
// continue-on-error, so cross-platform parity is aspirational — see
// .github/workflows/determinism.yml).
//
// Scenario (fully deterministic, fixed dt = 1/120):
//   * Spawn N = 64 DOTS steering agents on a fixed 8x8 grid via scene::spawn_agent,
//     all seeking the origin, and run physics::agents::update_agents for 240 ticks.
//   * Digest = FNV-1a/64 over the bytes of every agent's final TransformWS, read
//     back from the ECS in a STABLE entity-id order (Entity::raw ascending), NOT
//     storage order (the ECS swap-removes, so storage order is history-dependent).
//
// Assertions:
//   (1) CROSS-RUN: two in-process runs produce identical digests AND identical
//       TransformWS byte arrays (memcmp).
//   (2) GOLDEN: the digest equals a committed constexpr value produced once on
//       this machine (macOS/arm64). A drift fails loud — exactly what the lockstep
//       gate exists to catch.
//
// The TEST_CASE names carry "golden" + "determinism" so the determinism CI regex
// (golden|determinism|deterministic|replay|lockstep) selects them; the exe is also
// named psynder_golden so the exe-level ctest entry matches too.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/Types.h"
#include "jobs/JobSystem.h"
#include "math/Math.h"
#include "physics/agents/AgentComponents.h"
#include "physics/agents/AgentSystem.h"
#include "scene/GxComponents.h"
#include "scene/World.h"

using namespace psynder;
using namespace psynder::physics::agents;

namespace {

constexpr u32 kAgents = 64;          // 8x8 flock
constexpr u32 kTicks = 240;          // 2 s at 120 Hz
constexpr f32 kDt = 1.0f / 120.0f;   // fixed 120 Hz sim tick

// ── Deterministic FNV-1a/64 digest helper ─────────────────────────────────
// 64-bit FNV-1a: order-stable, no STL hashing (which differs across STLs), so
// the digest itself never introduces cross-platform divergence.
constexpr u64 kFnvOffset = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;

u64 fnv1a64(const void* data, usize n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    u64 h = kFnvOffset;
    for (usize i = 0; i < n; ++i) {
        h ^= static_cast<u64>(p[i]);
        h *= kFnvPrime;
    }
    return h;
}

// RAII pool guard: parallel_for is synchronous either way, but starting the real
// pool exercises the multi-thread path so we prove thread-count independence.
struct Pool {
    Pool() { jobs::JobSystem::Get().start(); }
    ~Pool() { jobs::JobSystem::Get().stop(); }
};

// Deterministic flock: 8x8 agents on a grid in the XZ plane, all seeking the
// origin, so they converge and must avoid one another on the way in. Returns the
// spawned entities in spawn order.
std::vector<Entity> make_flock(scene::World& w) {
    std::vector<Entity> out;
    out.reserve(kAgents);
    const u32 side = 8;
    const f32 spacing = 2.0f;
    for (u32 ix = 0; ix < side; ++ix) {
        for (u32 iz = 0; iz < side; ++iz) {
            scene::Agent a{};
            a.max_speed_mps = 3.0f;   // ~jog
            a.max_force = 8.0f;       // m/s^2
            a.radius_m = 0.4f;        // ~human capsule radius
            a.arrive_radius_m = 1.5f;
            const f32 x = (static_cast<f32>(ix) - 3.5f) * spacing + 12.0f;
            const f32 z = (static_cast<f32>(iz) - 3.5f) * spacing + 12.0f;
            out.push_back(scene::spawn_agent(w, a, {x, 0.0f, z}, {0.0f, 0.0f, 0.0f}));
        }
    }
    return out;
}

// Run `ticks` ticks of the open-field flock; return the final TransformWS array
// read back in STABLE entity-id order (Entity::raw ascending). Spawn order here is
// already id-ascending, but we sort explicitly so the digest is independent of the
// ECS's history-dependent storage order and of any future spawn reordering.
std::vector<scene::TransformWS> run_stable(u32 ticks) {
    scene::World w;
    std::vector<Entity> ents = make_flock(w);
    AgentScratch scratch;
    for (u32 t = 0; t < ticks; ++t) update_agents(w, scratch, kDt);

    std::sort(ents.begin(), ents.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });

    std::vector<scene::TransformWS> out;
    out.reserve(ents.size());
    for (Entity e : ents) out.push_back(*w.get<scene::TransformWS>(e));
    return out;
}

u64 digest_of(const std::vector<scene::TransformWS>& xs) {
    return fnv1a64(xs.data(), xs.size() * sizeof(scene::TransformWS));
}

// ── Committed golden ───────────────────────────────────────────────────────
// FNV-1a/64 over the 64-agent final TransformWS array (id-ordered) after 240
// ticks at dt = 1/120, on the AUTHORITATIVE config: macOS / arm64 / Release.
//
// The absolute digest is build-config-sensitive: the agent integrator is strict
// -fno-fast-math/-ffp-contract=off, but its neighbour set comes through the
// scene SpatialIndex distance compares, which are NOT strict-FP, so -O0 vs -O2
// can flip a borderline neighbour and shift the digest. (Debug builds on this
// same machine produce 0x38dabe47c4007b4f.) Both Release configs — the plain
// Release and the mac-release preset — agree, so the authoritative value is
// stable; we therefore only pin the absolute value on macOS/arm64/Release.
// Cross-platform parity remains aspirational (CI other-platforms continue-on-
// error). The cross-run determinism test below is the config/platform-agnostic
// hard gate; this absolute pin additionally catches behavioural drift on the
// authoritative platform. If it changes there, the sim drifted — investigate
// before bumping.
constexpr u64 kGoldenDigest = 0xe2aec015e1fc094full;  // macOS / arm64 / Release

// The authoritative config the absolute golden is pinned for.
#if defined(NDEBUG) && defined(__APPLE__) && defined(__aarch64__)
#  define PSY_GOLDEN_AUTHORITATIVE 1
#else
#  define PSY_GOLDEN_AUTHORITATIVE 0
#endif

}  // namespace

TEST_CASE("golden agent flock digest is cross-run determinism stable",
          "[golden][determinism][agents]") {
    Pool pool;
    const std::vector<scene::TransformWS> a = run_stable(kTicks);
    const std::vector<scene::TransformWS> b = run_stable(kTicks);
    REQUIRE(a.size() == kAgents);
    REQUIRE(b.size() == a.size());
    // Full byte array identical...
    REQUIRE(std::memcmp(a.data(), b.data(),
                        a.size() * sizeof(scene::TransformWS)) == 0);
    // ...and so are the digests.
    REQUIRE(digest_of(a) == digest_of(b));
}

TEST_CASE("golden agent flock digest matches committed determinism value",
          "[golden][determinism][agents]") {
    Pool pool;
    const std::vector<scene::TransformWS> a = run_stable(kTicks);
    const u64 d = digest_of(a);
    INFO("agent flock golden digest = 0x" << std::hex << d);
#if PSY_GOLDEN_AUTHORITATIVE
    // Authoritative config (macOS / arm64 / Release): pin the exact value to
    // catch behavioural drift in the lockstep-critical sim.
    REQUIRE(d == kGoldenDigest);
#else
    // Off the authoritative config the absolute value is expected to differ
    // (opt level / arch / compiler). The cross-run determinism test is the gate
    // here; we only assert the digest was produced (non-degenerate).
    REQUIRE(d != 0u);
#endif
}
