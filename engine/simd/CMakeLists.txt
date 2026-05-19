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
# __attribute__((target("avx512f,avx512bw,avx512vl"))) on each kernel so the
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
    # __attribute__((target("avx512f,avx512bw,avx512vl"))) per-function;
    # no extra compile flag is needed for that TU either — the attribute
    # unlocks the intrinsics locally. PSYNDER_ENABLE_AVX512 is kept as a
    # CMake option so downstream consumers can query whether the wide path
    # was requested, but it no longer adds a compiler flag.
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
