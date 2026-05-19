// SPDX-License-Identifier: MIT
// Psynder — AVX-512 wide-path kernel implementations. Lane 03 owns.
//
// Each function is decorated with __attribute__((target("avx512f,avx512bw,avx512vl")))
// so the compiler emits AVX-512 instructions ONLY inside these function bodies.
// The rest of psynder_simd compiles at the x86-64 baseline (SSE4.2) or the
// AVX2 fast path — no TU-wide -mavx512f flag is used. This is the fix for the
// SIGILL incident caused by the compiler auto-vectorising non-dispatched loops
// with AVX-512 instructions when the whole TU was compiled with -mavx512f.
//
// Guard: compiled only on x86-64 with GCC or Clang. MSVC does not have an
// equivalent per-function ISA attribute; MSVC users get the composed two-
// AVX2-halves fallback defined inline in Simd_internal.h until clang-cl is
// adopted as the Windows build path.
//
// Runtime gate: callers must check psynder::simd::current_tier() == Tier::Avx512
// (or rely on Dispatch.cpp's batched entry points) before invoking these.
//
// Build gate: PSYNDER_AVX512_ENABLED is set by the CMake option
// PSYNDER_ENABLE_AVX512. When OFF, this TU collapses to nothing and the
// composed fallback in Simd_internal.h is used everywhere.

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__clang__) || defined(__GNUC__)) && !defined(_MSC_VER) && defined(PSYNDER_AVX512_ENABLED)

#include "Simd_internal.h"

// Pull in <immintrin.h> for __m512 / _mm512_* intrinsics. The per-function
// target attribute makes these available within each decorated function body
// without requiring a TU-wide -mavx512f flag.
#include <immintrin.h>

namespace psynder::simd {

// Convenience macro — keeps the attribute in one place.
#define PSY_AVX512 __attribute__((target("avx512f,avx512bw,avx512vl")))

// f32x16 stores two AVX2 halves (defined in Simd_internal.h).  Inside the
// target-attributed bodies below the compiler may freely use _mm512_* because
// the attribute has enabled AVX-512F/BW/VL for each function scope.

PSY_AVX512 f32x16 add16(f32x16 a, f32x16 b) noexcept {
    // Reinterpret the two __m256 halves into a __m512, add, split back.
    __m512 va = _mm512_insertf32x8(_mm512_castps256_ps512(a.lo.v), a.hi.v, 1);
    __m512 vb = _mm512_insertf32x8(_mm512_castps256_ps512(b.lo.v), b.hi.v, 1);
    __m512 vr = _mm512_add_ps(va, vb);
    return f32x16{f32x8{_mm512_castps512_ps256(vr)},
                  f32x8{_mm512_extractf32x8_ps(vr, 1)}};
}

PSY_AVX512 f32x16 sub16(f32x16 a, f32x16 b) noexcept {
    __m512 va = _mm512_insertf32x8(_mm512_castps256_ps512(a.lo.v), a.hi.v, 1);
    __m512 vb = _mm512_insertf32x8(_mm512_castps256_ps512(b.lo.v), b.hi.v, 1);
    __m512 vr = _mm512_sub_ps(va, vb);
    return f32x16{f32x8{_mm512_castps512_ps256(vr)},
                  f32x8{_mm512_extractf32x8_ps(vr, 1)}};
}

PSY_AVX512 f32x16 mul16(f32x16 a, f32x16 b) noexcept {
    __m512 va = _mm512_insertf32x8(_mm512_castps256_ps512(a.lo.v), a.hi.v, 1);
    __m512 vb = _mm512_insertf32x8(_mm512_castps256_ps512(b.lo.v), b.hi.v, 1);
    __m512 vr = _mm512_mul_ps(va, vb);
    return f32x16{f32x8{_mm512_castps512_ps256(vr)},
                  f32x8{_mm512_extractf32x8_ps(vr, 1)}};
}

PSY_AVX512 f32x16 fma16(f32x16 a, f32x16 b, f32x16 c) noexcept {
    __m512 va = _mm512_insertf32x8(_mm512_castps256_ps512(a.lo.v), a.hi.v, 1);
    __m512 vb = _mm512_insertf32x8(_mm512_castps256_ps512(b.lo.v), b.hi.v, 1);
    __m512 vc = _mm512_insertf32x8(_mm512_castps256_ps512(c.lo.v), c.hi.v, 1);
    __m512 vr = _mm512_fmadd_ps(va, vb, vc);
    return f32x16{f32x8{_mm512_castps512_ps256(vr)},
                  f32x8{_mm512_extractf32x8_ps(vr, 1)}};
}

PSY_AVX512 f32x16 load_aligned16(const psynder::f32* p) noexcept {
    __m512 v = _mm512_load_ps(p);
    return f32x16{f32x8{_mm512_castps512_ps256(v)},
                  f32x8{_mm512_extractf32x8_ps(v, 1)}};
}

PSY_AVX512 f32x16 load_unaligned16(const psynder::f32* p) noexcept {
    __m512 v = _mm512_loadu_ps(p);
    return f32x16{f32x8{_mm512_castps512_ps256(v)},
                  f32x8{_mm512_extractf32x8_ps(v, 1)}};
}

PSY_AVX512 void store_aligned16(psynder::f32* p, f32x16 v) noexcept {
    __m512 vr = _mm512_insertf32x8(_mm512_castps256_ps512(v.lo.v), v.hi.v, 1);
    _mm512_store_ps(p, vr);
}

PSY_AVX512 void store_unaligned16(psynder::f32* p, f32x16 v) noexcept {
    __m512 vr = _mm512_insertf32x8(_mm512_castps256_ps512(v.lo.v), v.hi.v, 1);
    _mm512_storeu_ps(p, vr);
}

PSY_AVX512 f32x16 broadcast16(psynder::f32 s) noexcept {
    __m512 v = _mm512_set1_ps(s);
    return f32x16{f32x8{_mm512_castps512_ps256(v)},
                  f32x8{_mm512_extractf32x8_ps(v, 1)}};
}

PSY_AVX512 psynder::f32 reduce_add16(f32x16 a) noexcept {
    __m512 va = _mm512_insertf32x8(_mm512_castps256_ps512(a.lo.v), a.hi.v, 1);
    return _mm512_reduce_add_ps(va);
}

#undef PSY_AVX512

}  // namespace psynder::simd

#endif  // x86-64 Clang/GCC guard
