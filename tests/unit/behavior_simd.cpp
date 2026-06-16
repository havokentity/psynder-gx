// SPDX-License-Identifier: MIT
// Psynder-GX — Behavior IR SIMD back-end (engine/script/behavior/BehaviorSimd).
// The SIMD lane-batch execution of a BehaviorProgram must be BIT-IDENTICAL to
// the scalar BehaviorIR::execute interpreter for the same program + inputs. These
// tests run several programs (heal/threshold-select, projectile integration, and
// a multi-op arithmetic program exercising Div/Min/Max/Madd/Select) over N
// entities through BOTH paths and assert byte-for-byte equal output columns,
// including an entity count that is NOT a multiple of the SIMD width (so the
// scalar tail is exercised), plus run-to-run determinism.

#include "script/behavior/BehaviorIR.h"
#include "script/behavior/BehaviorSimd.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::script::behavior;

namespace {

// Stream layout for the projectile behavior (mirrors behavior_ir.cpp).
enum : u16 { PX = 0, PY = 1, PZ = 2, VX = 3, VY = 4, VZ = 5, NUM = 6 };

// Semi-implicit Euler under gravity on Y. vy += g*dt; p += v*dt.
BehaviorProgram projectile_program(f32 dt, f32 g) {
    BehaviorBuilder b(NUM);
    const u16 u_dt = b.uniform(dt);
    const u16 u_g = b.uniform(g);
    const u16 rdt = b.reg();
    const u16 rg = b.reg();
    b.load_uniform(rdt, u_dt);
    b.load_uniform(rg, u_g);

    const u16 rv = b.reg();
    b.load_stream(rv, VY);
    b.madd(rv, rv, rg, rdt);  // rv = rv + rg*rdt
    b.store_stream(VY, rv);

    const u16 rp = b.reg();
    const u16 rvv = b.reg();
    const u16 pos[3] = {PX, PY, PZ};
    const u16 vel[3] = {VX, VY, VZ};
    for (int axis = 0; axis < 3; ++axis) {
        b.load_stream(rp, pos[axis]);
        b.load_stream(rvv, vel[axis]);
        b.madd(rp, rp, rvv, rdt);
        b.store_stream(pos[axis], rp);
    }
    return b.build();
}

// hp <= 25 -> flag = 1 and heal by 50; otherwise untouched. (mirrors behavior_ir)
BehaviorProgram heal_program() {
    enum : u16 { HP = 0, FLAG = 1, NS = 2 };
    BehaviorBuilder b(NS);
    const u16 u_thresh = b.uniform(25.0f);
    const u16 u_heal = b.uniform(50.0f);
    const u16 rhp = b.reg(), rth = b.reg(), rflag = b.reg(), rheal = b.reg(),
              rnew = b.reg();
    b.load_stream(rhp, HP);
    b.load_uniform(rth, u_thresh);
    b.cmp_le(rflag, rhp, rth);
    b.store_stream(FLAG, rflag);
    b.load_uniform(rheal, u_heal);
    b.add(rheal, rhp, rheal);
    b.select(rnew, rflag, rheal, rhp);
    b.store_stream(HP, rnew);
    return b.build();
}

// A wide multi-op program over 4 streams exercising every core arithmetic op,
// the div-by-zero guard, min/max clamping, a compare, and a select:
//   a' = clamp( (a*2 + 1) / max(b, 1) , lo=0, hi=hicap )   // div, mul, add, madd-free
//   then if (a' < c)  out = a' + d   else out = a' - d      // cmp + select
//   stores a' back to stream 0 and the branch result to stream 1.
enum : u16 { SA = 0, SB = 1, SC = 2, SD = 3, MSTREAMS = 4 };
BehaviorProgram multi_op_program() {
    BehaviorBuilder b(MSTREAMS);
    const u16 u_two = b.uniform(2.0f);
    const u16 u_one = b.uniform(1.0f);
    const u16 u_zero = b.uniform(0.0f);
    const u16 u_hicap = b.uniform(7.5f);

    const u16 ra = b.reg(), rb = b.reg(), rc = b.reg(), rd = b.reg();
    const u16 rtwo = b.reg(), rone = b.reg(), rzero = b.reg(), rhi = b.reg();
    const u16 rtmp = b.reg(), rden = b.reg(), rsel = b.reg(), rflag = b.reg(),
              rlo = b.reg(), rhipath = b.reg();

    b.load_stream(ra, SA);
    b.load_stream(rb, SB);
    b.load_stream(rc, SC);
    b.load_stream(rd, SD);
    b.load_uniform(rtwo, u_two);
    b.load_uniform(rone, u_one);
    b.load_uniform(rzero, u_zero);
    b.load_uniform(rhi, u_hicap);

    // tmp = a*2 + 1   (madd: rtmp = rone + ra*rtwo)
    b.madd(rtmp, rone, ra, rtwo);
    // den = max(b, 1)  ; tmp = tmp / den
    b.max(rden, rb, rone);
    b.div(rtmp, rtmp, rden);
    // clamp(tmp, 0, hicap)
    b.max(rtmp, rtmp, rzero);
    b.min(rtmp, rtmp, rhi);
    b.store_stream(SA, rtmp);

    // flag = tmp < c ; lo = tmp + d ; hi = tmp - d ; out = flag ? lo : hi
    b.cmp_lt(rflag, rtmp, rc);
    b.add(rlo, rtmp, rd);
    b.sub(rhipath, rtmp, rd);
    b.select(rsel, rflag, rlo, rhipath);
    b.store_stream(SB, rsel);
    return b.build();
}

// Flatten a chunk's streams into one contiguous f32 buffer for memcmp.
std::vector<f32> flatten(const BehaviorChunk& c) {
    std::vector<f32> out;
    for (const auto& s : c.streams) out.insert(out.end(), s.begin(), s.end());
    return out;
}

bool bit_equal(const std::vector<f32>& a, const std::vector<f32>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0;
}

// Run a program over `count` entities seeded by `seed`, through both the scalar
// interpreter and the SIMD back-end on two independent chunks, and assert the
// output columns are byte-for-byte equal.
void assert_simd_equals_scalar(const BehaviorProgram& prog, u16 nstreams,
                               usize count, int ticks,
                               void (*seed)(BehaviorChunk&)) {
    BehaviorChunk scalar_chunk;
    BehaviorChunk simd_chunk;
    scalar_chunk.configure(nstreams, count);
    simd_chunk.configure(nstreams, count);
    seed(scalar_chunk);
    seed(simd_chunk);

    for (int t = 0; t < ticks; ++t) {
        execute(prog, scalar_chunk);
        execute_simd(prog, simd_chunk);
    }

    const std::vector<f32> a = flatten(scalar_chunk);
    const std::vector<f32> b = flatten(simd_chunk);
    REQUIRE(bit_equal(a, b));
}

void seed_projectile(BehaviorChunk& c) {
    for (usize i = 0; i < c.count; ++i) {
        c.stream(VX)[i] = static_cast<f32>(i % 7) * 0.5f;
        c.stream(VY)[i] = 5.0f + static_cast<f32>(i % 3);
        c.stream(PY)[i] = 2.0f;
    }
}

void seed_heal(BehaviorChunk& c) {
    enum : u16 { HP = 0 };
    for (usize i = 0; i < c.count; ++i)
        c.stream(HP)[i] = static_cast<f32>((i * 13) % 90);  // 0..89, straddles 25
}

void seed_multi(BehaviorChunk& c) {
    for (usize i = 0; i < c.count; ++i) {
        c.stream(SA)[i] = static_cast<f32>(i % 11) - 4.0f;   // negatives -> clamp lo
        c.stream(SB)[i] = static_cast<f32>(i % 5) - 2.0f;    // includes 0 -> max(.,1)
        c.stream(SC)[i] = static_cast<f32>(i % 9) * 0.5f;
        c.stream(SD)[i] = static_cast<f32>(i % 3) + 0.25f;
    }
}

}  // namespace

TEST_CASE("behavior-simd: heal-threshold-select matches the scalar interpreter bitwise",
          "[behavior][simd][determinism]") {
    const BehaviorProgram prog = heal_program();
    // 13 entities: not a multiple of the SIMD width, so the scalar tail runs.
    assert_simd_equals_scalar(prog, /*nstreams=*/2, /*count=*/13, /*ticks=*/3,
                              &seed_heal);
}

TEST_CASE("behavior-simd: projectile integration matches the scalar interpreter bitwise",
          "[behavior][simd][gameplay]") {
    const BehaviorProgram prog = projectile_program(1.0f / 144.0f, -9.81f);
    // Exact multiple of the width AND a ragged count, both over many ticks.
    assert_simd_equals_scalar(prog, NUM, /*count=*/64, /*ticks=*/200, &seed_projectile);
    assert_simd_equals_scalar(prog, NUM, /*count=*/67, /*ticks=*/200, &seed_projectile);
}

TEST_CASE("behavior-simd: multi-op arithmetic program matches the scalar interpreter bitwise",
          "[behavior][simd][determinism]") {
    const BehaviorProgram prog = multi_op_program();
    // 70 entities exercises full blocks plus a 6-lane tail (70 % 8 == 6).
    assert_simd_equals_scalar(prog, MSTREAMS, /*count=*/70, /*ticks=*/1, &seed_multi);
    // Also a single full block and a count below one width.
    assert_simd_equals_scalar(prog, MSTREAMS, /*count=*/8, /*ticks=*/1, &seed_multi);
    assert_simd_equals_scalar(prog, MSTREAMS, /*count=*/5, /*ticks=*/1, &seed_multi);
}

TEST_CASE("behavior-simd: the div-by-zero guard and select land on expected values",
          "[behavior][simd]") {
    // Sanity-anchor the SIMD output to concrete numbers (not just equality to the
    // scalar path) so a shared bug in both interpreters cannot pass silently.
    const BehaviorProgram prog = multi_op_program();
    BehaviorChunk c;
    c.configure(MSTREAMS, 3);
    // entity 0: a=3, b=0 -> den=max(0,1)=1, tmp=(3*2+1)/1=7, clamp->7 (hicap 7.5)
    //           c=10 -> 7<10 true -> out = 7 + d(=2) = 9
    // entity 1: a=10,b=4 -> tmp=(21)/4=5.25, clamp->5.25 ; c=1 -> 5.25<1 false ->
    //           out = 5.25 - d(=0.5) = 4.75
    // entity 2: a=-5,b=2 -> tmp=(-9)/2=-4.5, clamp lo ->0 ; c=3 -> 0<3 true ->
    //           out = 0 + d(=1) = 1
    const f32 sa[3] = {3.0f, 10.0f, -5.0f};
    const f32 sb[3] = {0.0f, 4.0f, 2.0f};
    const f32 sc[3] = {10.0f, 1.0f, 3.0f};
    const f32 sd[3] = {2.0f, 0.5f, 1.0f};
    for (usize i = 0; i < 3; ++i) {
        c.stream(SA)[i] = sa[i];
        c.stream(SB)[i] = sb[i];
        c.stream(SC)[i] = sc[i];
        c.stream(SD)[i] = sd[i];
    }
    execute_simd(prog, c);

    REQUIRE(c.stream(SA)[0] == Catch::Approx(7.0f));
    REQUIRE(c.stream(SA)[1] == Catch::Approx(5.25f));
    REQUIRE(c.stream(SA)[2] == Catch::Approx(0.0f));
    REQUIRE(c.stream(SB)[0] == Catch::Approx(9.0f));
    REQUIRE(c.stream(SB)[1] == Catch::Approx(4.75f));
    REQUIRE(c.stream(SB)[2] == Catch::Approx(1.0f));
}

TEST_CASE("behavior-simd: strided columns match the contiguous scalar result bitwise",
          "[behavior][simd]") {
    // Drive the SIMD path through interleaved (AoS-style) storage via StreamColumn
    // strides, proving the per-lane gather/scatter matches the scalar interpreter's
    // own strided execute. Two streams interleaved into one array: [hp, flag] pairs.
    const BehaviorProgram prog = heal_program();
    constexpr usize count = 19;  // ragged: exercises the tail under striding too

    std::vector<f32> interleaved(count * 2, 0.0f);
    for (usize i = 0; i < count; ++i)
        interleaved[i * 2] = static_cast<f32>((i * 13) % 90);  // hp at stride-2

    std::vector<f32> scalar_buf = interleaved;
    std::vector<f32> simd_buf = interleaved;

    StreamColumn scalar_cols[2] = {{scalar_buf.data() + 0, 2}, {scalar_buf.data() + 1, 2}};
    StreamColumn simd_cols[2] = {{simd_buf.data() + 0, 2}, {simd_buf.data() + 1, 2}};

    execute(prog, std::span<const StreamColumn>(scalar_cols, 2), count);
    execute_simd(prog, std::span<const StreamColumn>(simd_cols, 2), count);

    REQUIRE(scalar_buf.size() == simd_buf.size());
    REQUIRE(std::memcmp(scalar_buf.data(), simd_buf.data(),
                        scalar_buf.size() * sizeof(f32)) == 0);
}

TEST_CASE("behavior-simd: execution is bit-deterministic across runs",
          "[behavior][simd][determinism]") {
    const BehaviorProgram prog = projectile_program(1.0f / 120.0f, -9.81f);
    const auto run = [&]() {
        BehaviorChunk c;
        c.configure(NUM, 67);  // ragged count: tail participates each run
        seed_projectile(c);
        for (int t = 0; t < 200; ++t) execute_simd(prog, c);
        return flatten(c);
    };
    const std::vector<f32> a = run();
    const std::vector<f32> b = run();
    REQUIRE(bit_equal(a, b));
}
