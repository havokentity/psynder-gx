# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_net — Lane 18 (rUDP, lockstep, snapshot delta, 128-tick capable,
# lag comp rewind, demos). Voice chat lives in voice/ as lane 19.
# Excludes voice/ from the glob via filter below.

file(GLOB_RECURSE _net_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
# Drop voice/* — owned by lane 19 (psynder_net_voice).
list(FILTER _net_sources EXCLUDE REGEX "/voice/")

if(NOT _net_sources)
    set(_stub "${CMAKE_CURRENT_BINARY_DIR}/_psynder_net_stub.cpp")
    if(NOT EXISTS "${_stub}")
        file(WRITE "${_stub}" "namespace psynder::net_stub { void anchor() {} }\n")
    endif()
    set(_net_sources "${_stub}")
endif()

add_library(psynder_net STATIC ${_net_sources})
add_library(psynder::net ALIAS psynder_net)
target_link_libraries(psynder_net PUBLIC
    psynder_common psynder_core psynder_math psynder_jobs psynder_scene
)
if(PSYNDER_GX_PLATFORM_WIN32 OR PSYNDER_PLATFORM_WIN32)
    target_link_libraries(psynder_net PUBLIC ws2_32)
endif()

# Voice subdir = lane 19.
add_subdirectory(voice)
