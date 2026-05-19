// SPDX-License-Identifier: MIT OR Apache-2.0
//
// samples/00_clear/main.cpp
//
// M0 deliverable: animated clear color on Win/Linux (Vulkan) + macOS
// (native Metal) from one source. Wave-0 placeholder: validates the
// build wires up. The lane 07 (gpu) + 23/24/25 (platform) agents fill in
// the Vulkan / Metal device + surface + present, and this sample lights
// up after both lands.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(PSYNDER_GX_BACKEND_VULKAN) || defined(PSYNDER_GX_BACKEND_METAL)
    // Real entry point after lane 07 + lane 23/24/25 wire the platform device.
    // For Wave 0, we just sanity-print and exit.
#endif

namespace {
int parse_smoke_frames(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--smoke-frames=", 15) == 0) {
            return std::atoi(argv[i] + 15);
        }
    }
    return 0;
}
}

int main(int argc, char** argv) {
    int smoke_frames = parse_smoke_frames(argc, argv);

#if defined(PSYNDER_GX_BACKEND_METAL)
    std::printf("[psynder-gx] sample_00_clear — Metal backend (macOS Apple Silicon)\n");
#elif defined(PSYNDER_GX_BACKEND_VULKAN)
    std::printf("[psynder-gx] sample_00_clear — Vulkan backend (Win/Linux)\n");
#else
    std::printf("[psynder-gx] sample_00_clear — no GPU backend selected\n");
#endif

    if (smoke_frames > 0) {
        std::printf("[psynder-gx] smoke run: %d frames (Wave 0 placeholder — render path lights up after lane 07 + lane 23/24/25 land)\n", smoke_frames);
        return 0;
    }

    std::printf("[psynder-gx] interactive mode not yet wired (Wave 0)\n");
    return 0;
}
