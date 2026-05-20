# SPDX-License-Identifier: MIT
# psynder_audio — Lane 14. Owns: 32-ch software mixer, HRTF, FFT convolution reverb, FDN.
#
# The `builtin/` subdirectory holds:
#   - bake_short_room.cpp  — offline tool, NOT linked into the runtime.
#                            See its file comment for build instructions.
#   - short_room.bin       — pre-baked 24000-sample float32 IR (0.5 s @ 48 kHz).
#                            Loaded at runtime by audio/internal/MixerCore.h.
#
# psynder_add_lane globs *.cpp recursively, so we strip the baker out post-hoc
# (no compile errors otherwise, but we keep it out of the static archive).
psynder_add_lane(audio LINKS psynder_core psynder_math psynder_simd psynder_jobs psynder_asset)

# Drop the offline baker from the static library — it has its own main() and
# is meant to be built standalone when the IR needs regenerating.
get_target_property(_AUDIO_SRC psynder_audio SOURCES)
if(_AUDIO_SRC)
    list(FILTER _AUDIO_SRC EXCLUDE REGEX ".*/builtin/bake_short_room\\.cpp$")
    set_target_properties(psynder_audio PROPERTIES SOURCES "${_AUDIO_SRC}")
endif()

# Bundle the short_room.bin alongside the engine in the build output so sample
# binaries / tests can find it via the engine root. The path
# engine/audio/builtin/short_room.bin is a stable relative location.
target_compile_definitions(psynder_audio PRIVATE
    PSY_AUDIO_RIR_RELATIVE_PATH="engine/audio/builtin/short_room.bin")

# Copy short_room.bin into the build tree at <BUILD>/engine/audio/builtin/
# so runtime loaders using PSY_AUDIO_RIR_RELATIVE_PATH (resolved relative to
# the running binary's working directory) actually find it.  Without this
# step the file is only present in the source tree, and any test/sample
# launched with CWD=<BUILD> would silently fall back to the synthetic IR
# (caught by Copilot's PR #9 review).  Using `configure_file` keeps the
# step idempotent and rebuilds it whenever the source asset changes.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin/short_room.bin"
    "${CMAKE_BINARY_DIR}/engine/audio/builtin/short_room.bin"
    COPYONLY)
