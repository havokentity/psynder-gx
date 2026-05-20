# SPDX-License-Identifier: MIT
# psynder_simd — Lane 03. Owns: f32x4/x8, i32x4/x8, masks; SSE/AVX2/NEON
# kernels; runtime dispatch; AVX-512 per-function kernels.
psynder_add_lane(simd LINKS psynder_math psynder_core)

# ─── SIMD compile flags ──────────────────────────────────────────────────
# AVX-2 fast path: compiled TU-wide with -mavx2 -mfma. This is safe because
# AVX2 has been in Intel CPUs since Haswell (2013) and AMD since Ryzen 1
# (2017); runtime dispatch still gates usage on CpuFeatures::avx2.
#
# AVX-512 wide path: NOT a TU-wide flag any more. Simd_avx512.cpp carries
# __attribute__((target("avx512f,avx512bw,avx512vl,avx512dq"))) on each kernel so the
# compiler emits AVX-512 instructions only inside those function bodies.
# The rest of psynder_simd compiles at SSE4.2 / AVX2 baseline — auto-
# vectorisation never touches AVX-512 elsewhere (the SIGILL fix).
#
# MSVC: /arch:AVX512 would re-introduce the TU-wide problem because MSVC
# has no per-function ISA attribute. MSVC users get the composed two-AVX2-
# halves fallback defined in Simd_internal.h until clang-cl is adopted.

if(PSYNDER_ARCH_X64 AND NOT MSVC)
    # SSE4.2 baseline — already in the bootstrap, but make it explicit
    # so a future toolchain default change doesn't silently drop us.
    target_compile_options(psynder_simd PUBLIC -msse4.2)

    if(PSYNDER_ENABLE_AVX2)
        # AVX2 + FMA fast path. These define __AVX__, __AVX2__, __FMA__
        # which Simd_internal.h keys off.
        target_compile_options(psynder_simd PUBLIC -mavx2 -mfma)
    endif()

    # No TU-wide -mavx512f. The wide-path kernels in Simd_avx512.cpp carry
    # __attribute__((target("avx512f,avx512bw,avx512vl,avx512dq"))) per-function;
    # no extra compile flag is needed for that TU either — the attribute
    # unlocks the intrinsics locally. PSYNDER_ENABLE_AVX512 is kept as a
    # CMake option so downstream consumers can query whether the wide path
    # was requested, but it no longer adds a compiler flag.
    #
    # Dependency: AVX-512 requires AVX2 because Simd_avx512.cpp uses
    # `f32x8::v` (the __m256 native layout) directly to compose / split
    # __m512 values. With AVX2 disabled, `__AVX__` is undefined and `f32x8`
    # falls back to a two-__m128-halves struct that does NOT have a `.v`
    # member, so Simd_avx512.cpp would fail to compile. Refuse the combo
    # outright rather than silently producing a broken build.
    if(PSYNDER_ENABLE_AVX512 AND NOT PSYNDER_ENABLE_AVX2)
        message(FATAL_ERROR
            "PSYNDER_ENABLE_AVX512=ON requires PSYNDER_ENABLE_AVX2=ON — "
            "the AVX-512 wide path composes __m512 from two __m256 halves "
            "exposed via f32x8::v, which only exists when __AVX__ is defined. "
            "Either also enable AVX2, or disable AVX-512.")
    endif()
    if(PSYNDER_ENABLE_AVX512)
        target_compile_definitions(psynder_simd PUBLIC PSYNDER_AVX512_ENABLED)
    endif()
elseif(PSYNDER_ARCH_X64 AND MSVC)
    # MSVC: /arch:AVX2 enables the fast path. No /arch:AVX512 here — see
    # the note above about MSVC lacking per-function ISA attributes.
    if(PSYNDER_ENABLE_AVX2)
        target_compile_options(psynder_simd PUBLIC /arch:AVX2)
    endif()
endif()

if(PSYNDER_ARCH_ARM64 AND NOT MSVC)
    # NEON is implicit on aarch64; nothing to set. Apple Silicon has no
    # 256-bit widening, so f32x8 stays as two-halves on this arch — the
    # internal header handles that automatically.
endif()
