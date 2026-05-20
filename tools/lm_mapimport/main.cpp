// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 24 lm_mapimport CLI entrypoint.
//
// Thin argv wrapper around psy::lm_mapimport::import().

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "LmMapImport.h"

namespace {

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "lm_mapimport - Psynder-GX Quake .map -> .psimport scene importer (Lane 24)\n"
                 "\n"
                 "USAGE:\n"
                 "    lm_mapimport --in <MAP> --out <PSIMPORT> [OPTIONS]\n"
                 "\n"
                 "Imports a Quake / TrenchBroom .map (standard or Valve-220) into a\n"
                 "documented, versioned binary scene: convex plane-set brushes (with\n"
                 "full texture projection) plus entity key/value dictionaries.\n"
                 "\n"
                 "OPTIONS:\n"
                 "    --in <PATH>        source .map\n"
                 "    --out <PATH>       output .psimport path\n"
                 "    --scale <FLOAT>    uniform scale applied to brush geometry (default 1.0)\n"
                 "    --force            overwrite an existing output file\n"
                 "    --print-stats      dump entity / brush / face counts to stdout\n"
                 "    --quiet            suppress the final summary line\n"
                 "    -h, --help         show this message\n"
                 "\n"
                 "UNITS: 1 world unit = 1 metre. --scale rescales brush planes; entity\n"
                 "       property strings (e.g. \"origin\") are preserved verbatim.\n"
                 "DETERMINISM: identical input produces byte-identical output.\n");
}

}  // namespace

int main(int argc, char** argv) {
    psy::lm_mapimport::ImportOptions opts;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next_arg = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lm_mapimport: error: %s requires an argument\n", name);
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
            // Locale-independent parse (matches the shared .map parser).
            float v = 0.0f;
            if (!psy::lmtools::parse_float(p, v) || !(v > 0.0f)) {
                std::fprintf(stderr, "lm_mapimport: error: --scale must be a positive float\n");
                return 2;
            }
            opts.scale = v;
        } else if (a == "--force") {
            opts.force_overwrite = true;
        } else if (a == "--print-stats") {
            opts.print_stats = true;
        } else if (a == "--quiet") {
            opts.quiet = true;
        } else {
            std::fprintf(stderr, "lm_mapimport: error: unrecognised argument '%s'\n", argv[i]);
            print_usage(stderr);
            return 2;
        }
    }

    if (opts.input_path.empty() || opts.output_path.empty()) {
        std::fprintf(stderr, "lm_mapimport: error: --in and --out are required\n");
        print_usage(stderr);
        return 2;
    }

    psy::lm_mapimport::ImportStats stats;
    if (!psy::lm_mapimport::import(opts, &stats)) {
        return 1;
    }

    if (opts.print_stats) {
        std::fprintf(stdout,
                     "lm_mapimport stats:\n"
                     "  bytes_written       %llu\n"
                     "  entities            %u\n"
                     "  brushes             %u\n"
                     "  faces               %u\n"
                     "  point_entities      %u\n"
                     "  source_hash         0x%08x\n",
                     static_cast<unsigned long long>(stats.bytes_written),
                     stats.entity_count,
                     stats.brush_count,
                     stats.face_count,
                     stats.point_entity_count,
                     stats.source_hash);
    } else if (!opts.quiet) {
        std::fprintf(stdout,
                     "lm_mapimport: wrote %llu bytes to '%s' (%u entities, %u brushes, %u faces)\n",
                     static_cast<unsigned long long>(stats.bytes_written),
                     opts.output_path.c_str(),
                     stats.entity_count,
                     stats.brush_count,
                     stats.face_count);
    }
    return 0;
}
