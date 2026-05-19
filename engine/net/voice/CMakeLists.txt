# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_net_voice — Lane 19.
# Opus codec, server-side mixing per recipient, VAD, push-to-talk.
# Same UDP transport as gameplay netcode; voice on its own unreliable
# channel with sequence numbers, no retransmission. See DESIGN §10.9.

# ─── Vendor Opus via FetchContent ─────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
    GIT_SHALLOW    TRUE
)

# Suppress Opus's own tests / docs / demos from polluting our build
set(OPUS_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(opus)

# ─── Lane target ─────────────────────────────────────────────────────────
psynder_add_lane(net_voice
    LINKS
        psynder_core psynder_jobs psynder_net psynder_audio
        opus
)

# Voice uses Opus MDCT; -fno-fast-math is mandatory for bit-exact decode.
# Mirror the same flag set as psynder_net (lane 18).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(psynder_net_voice PRIVATE
        -fno-fast-math
        -ffp-contract=off
    )
elseif(MSVC)
    target_compile_options(psynder_net_voice PRIVATE /fp:strict)
endif()

# Silence -Wold-style-cast warnings that originate inside Opus's own C macros.
# We treat the Opus include directory as a SYSTEM path so the compiler skips
# warning diagnostics for headers under it.
get_target_property(_opus_inc opus INTERFACE_INCLUDE_DIRECTORIES)
if(_opus_inc)
    target_include_directories(psynder_net_voice SYSTEM PRIVATE ${_opus_inc})
endif()
