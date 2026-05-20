// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 24 lm_qbsp CLI entrypoint.
//
// Thin argv wrapper around psy::lm_qbsp::compile().

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "LmQbsp.h"

namespace {

void print_usage(std::FILE* out) {
    std::fprintf(
        out,
        "lm_qbsp - Psynder-GX BSP compiler (Lane 24)\n"
        "\n"
        "USAGE:\n"
        "    lm_qbsp --in <MAP> --out <PSYBSP> [OPTIONS]\n"
        "\n"
        "Compiles a Quake-style .map brush set into the engine .psybsp format\n"
        "(nodes / leaves / faces / vertices / indices + a cell-visibility PVS).\n"
        "\n"
        "OPTIONS:\n"
        "    --in <PATH>        source .map (standard or Valve-220 brushes)\n"
        "    --out <PATH>       output .psybsp path\n"
        "    --scale <FLOAT>    uniform scale applied to source coordinates (default 1.0)\n"
        "    --force            overwrite an existing output file\n"
        "    --print-stats      dump node/leaf/face/cluster counts to stdout\n"
        "    --quiet            suppress the final summary line\n"
        "    -h, --help         show this message\n"
        "\n"
        "UNITS:\n"
        "    1 world unit = 1 metre. .map coordinates are read as metres; pass\n"
        "    --scale to convert a source authored in other units.\n"
        "\n"
        "DETERMINISM:\n"
        "    Compiling twice from identical input produces byte-identical output.\n");
}

}  // namespace

int main(int argc, char** argv) {
    psy::lm_qbsp::CompileOptions opts;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next_arg = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lm_qbsp: error: %s requires an argument\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            print_usage(stdout);
            return 0;
        } else if (a == "--in") {
            const char* p = next_arg("--in");
            if (!p)
                return 2;
            opts.input_path = p;
        } else if (a == "--out") {
            const char* p = next_arg("--out");
            if (!p)
                return 2;
            opts.output_path = p;
        } else if (a == "--scale") {
            const char* p = next_arg("--scale");
            if (!p)
                return 2;
            char* end = nullptr;
            const double v = std::strtod(p, &end);
            if (end == p || *end != '\0' || !(v > 0.0)) {
                std::fprintf(stderr, "lm_qbsp: error: --scale must be a positive float\n");
                return 2;
            }
            opts.scale = static_cast<float>(v);
        } else if (a == "--force") {
            opts.force_overwrite = true;
        } else if (a == "--print-stats") {
            opts.print_stats = true;
        } else if (a == "--quiet") {
            opts.quiet = true;
        } else {
            std::fprintf(stderr, "lm_qbsp: error: unrecognised argument '%s'\n", argv[i]);
            print_usage(stderr);
            return 2;
        }
    }

    if (opts.input_path.empty() || opts.output_path.empty()) {
        std::fprintf(stderr, "lm_qbsp: error: --in and --out are required\n");
        print_usage(stderr);
        return 2;
    }

    psy::lm_qbsp::CompileStats stats;
    if (!psy::lm_qbsp::compile(opts, &stats)) {
        return 1;
    }

    if (opts.print_stats) {
        std::fprintf(stdout,
                     "lm_qbsp stats:\n"
                     "  bytes_written    %llu\n"
                     "  brushes          %u\n"
                     "  nodes            %u\n"
                     "  leaves           %u (solid %u, empty %u)\n"
                     "  faces            %u\n"
                     "  vertices         %u\n"
                     "  indices          %u\n"
                     "  clusters         %u (pvs_row_bytes %u)\n"
                     "  bounds_min       (%g, %g, %g)\n"
                     "  bounds_max       (%g, %g, %g)\n"
                     "  source_hash      0x%08x\n"
                     "  leaked           %s\n",
                     static_cast<unsigned long long>(stats.bytes_written),
                     stats.brush_count,
                     stats.node_count,
                     stats.leaf_count,
                     stats.solid_leaf_count,
                     stats.empty_leaf_count,
                     stats.face_count,
                     stats.vertex_count,
                     stats.index_count,
                     stats.cluster_count,
                     stats.pvs_row_bytes,
                     static_cast<double>(stats.bounds_min[0]),
                     static_cast<double>(stats.bounds_min[1]),
                     static_cast<double>(stats.bounds_min[2]),
                     static_cast<double>(stats.bounds_max[0]),
                     static_cast<double>(stats.bounds_max[1]),
                     static_cast<double>(stats.bounds_max[2]),
                     stats.source_hash,
                     stats.leaked ? "yes" : "no");
    } else if (!opts.quiet) {
        std::fprintf(
            stdout,
            "lm_qbsp: wrote %llu bytes to '%s' (%u nodes, %u leaves, %u faces, %u clusters)\n",
            static_cast<unsigned long long>(stats.bytes_written),
            opts.output_path.c_str(),
            stats.node_count,
            stats.leaf_count,
            stats.face_count,
            stats.cluster_count);
    }
    return 0;
}
