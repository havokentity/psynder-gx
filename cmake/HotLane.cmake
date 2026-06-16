# SPDX-License-Identifier: MIT
# Centralized hot-lane / determinism compiler contracts.
#
# Applying these as build flags (not just policy comments in code) is what
# actually enforces the engine's determinism + hot-path rules across compilers
# and CI — see AGENTS.md "Hard rules" and DESIGN-PSYNDER-GX.md §14.

include_guard(GLOBAL)

# Strict, deterministic floating point for lockstep-sensitive lanes (physics,
# netcode, and — later — generated gameplay TUs). No fast-math, no fused
# multiply-add contraction, IEEE-754 semantics. Combined with Jolt's
# CROSS_PLATFORM_DETERMINISTIC this is the foundation of bit-identical ticks
# across arm64 / x86_64 and across the client/server binaries.
#
# This is the single source of truth — lanes call it instead of hand-rolling
# per-compiler flags (which is how the physics lane previously missed the MSVC
# `/fp:strict` branch that the net lane had, a real cross-platform determinism
# hole).
function(psynder_determinism_fp target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -fno-fast-math -ffp-contract=off)
    elseif(MSVC)
        # /fp:strict disables fused-mac and assumes IEEE-754 semantics.
        target_compile_options(${target} PRIVATE /fp:strict)
    endif()
endfunction()

# Full hot-lane contract: determinism FP plus no-exceptions / no-RTTI for the
# renderer / scene / physics / audio-mixer / netcode hot paths (AGENTS.md).
#
# NOT yet applied wholesale: retrofitting -fno-exceptions / -fno-rtti onto an
# existing lane fails to compile if that lane still uses exceptions or RTTI, so
# each lane needs an exception/RTTI-freedom audit before opting in. Until then,
# lanes should at least call psynder_determinism_fp(). Track adoption per lane.
function(psynder_hot_lane target)
    psynder_determinism_fp(${target})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /GR-)
        # MSVC exception model is set globally; per-target /EHs-c- can conflict
        # with the toolchain default, so exception removal on MSVC is deferred
        # to the global exception-policy decision.
    endif()
endfunction()
