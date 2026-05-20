// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 05 psymesh_cook CLI entrypoint.
//
// Thin argv wrapper around psy::psymesh::cook().

#include "PsyMeshCook.h"
#include "PsyMeshFormat.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

void print_usage(std::FILE* out) {
    std::fprintf(out,
        "psymesh_cook — Psynder-GX offline mesh cooker (Lane 05)\n"
        "\n"
        "USAGE:\n"
        "    psymesh_cook --in <PATH> --out <PATH> [OPTIONS]\n"
        "\n"
        "INPUT FORMATS:\n"
        "    .obj                       hand-rolled parser; triangles only\n"
        "    .gltf / .glb               recognised but parser not yet wired (M2)\n"
        "\n"
        "OPTIONS:\n"
        "    --in <PATH>                input file (extension picks parser)\n"
        "    --out <PATH>               output .psymesh path\n"
        "    --scale <FLOAT>            uniform scale applied to positions (default 1.0).\n"
        "                               Positions are stored in METRES; pass 0.01 for cm sources.\n"
        "    --center-pivot             translate so the mesh's AABB centre sits at origin\n"
        "    --no-tangents              skip MikkT-equivalent tangent computation (default ON)\n"
        "    --gen-uv2-lightmap         emit a UV1 channel (M2 stub mirrors UV0)\n"
        "    --force                    overwrite an existing output file\n"
        "    --print-stats              dump byte counts + dedup ratio + source_hash to stdout\n"
        "    --quiet                    suppress final summary line on stdout\n"
        "    -h, --help                 show this message\n"
        "\n"
        "UNITS:\n"
        "    1 world unit = 1 metre. Real metric constants are mandatory across the engine.\n"
        "\n"
        "DETERMINISM:\n"
        "    Building twice on the same input MUST produce byte-identical output.\n"
        "    Verify with: psymesh_cook --in foo.obj --out a.psymesh && \\\n"
        "                 psymesh_cook --in foo.obj --out b.psymesh --force && \\\n"
        "                 cmp a.psymesh b.psymesh\n");
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    const std::size_t off = s.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[off + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

bool detect_input_kind(const std::string& path, psy::psymesh::InputKind& kind, std::string& err) {
    if (ends_with_ci(path, ".obj")) {
        kind = psy::psymesh::InputKind::Obj;
        return true;
    }
    if (ends_with_ci(path, ".gltf") || ends_with_ci(path, ".glb")) {
        kind = psy::psymesh::InputKind::Gltf;
        return true;
    }
    err = "unrecognised input extension (expected .obj / .gltf / .glb): " + path;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    psy::psymesh::CookOptions opts;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next_arg = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "psymesh_cook: error: %s requires an argument\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            print_usage(stdout);
            return 0;
        } else if (a == "--in") {
            const char* p = next_arg("--in");
            if (!p) return 2;
            opts.input_path = p;
        } else if (a == "--out") {
            const char* p = next_arg("--out");
            if (!p) return 2;
            opts.output_path = p;
        } else if (a == "--scale") {
            const char* p = next_arg("--scale");
            if (!p) return 2;
            char* end = nullptr;
            const double v = std::strtod(p, &end);
            if (end == p || *end != '\0' || !(v > 0.0)) {
                std::fprintf(stderr, "psymesh_cook: error: --scale must be a positive float\n");
                return 2;
            }
            opts.scale = static_cast<float>(v);
        } else if (a == "--center-pivot") {
            opts.center_pivot = true;
        } else if (a == "--no-tangents") {
            opts.compute_tangents = false;
        } else if (a == "--gen-uv2-lightmap") {
            opts.gen_uv2_lightmap_stub = true;
        } else if (a == "--force") {
            opts.force_overwrite = true;
        } else if (a == "--print-stats") {
            opts.print_stats = true;
        } else if (a == "--quiet") {
            opts.quiet = true;
        } else {
            std::fprintf(stderr, "psymesh_cook: error: unrecognised argument '%s'\n", argv[i]);
            print_usage(stderr);
            return 2;
        }
    }

    if (opts.input_path.empty() || opts.output_path.empty()) {
        std::fprintf(stderr, "psymesh_cook: error: --in and --out are required\n");
        print_usage(stderr);
        return 2;
    }
    std::string err;
    if (!detect_input_kind(opts.input_path, opts.input_kind, err)) {
        std::fprintf(stderr, "psymesh_cook: error: %s\n", err.c_str());
        return 2;
    }

    psy::psymesh::CookStats stats;
    if (!psy::psymesh::cook(opts, &stats)) {
        return 1;
    }

    if (opts.print_stats) {
        std::fprintf(stdout,
            "psymesh_cook stats:\n"
            "  bytes_written       %llu\n"
            "  source_vertices     %u\n"
            "  cooked_vertices     %u\n"
            "  index_count         %u\n"
            "  submesh_count       %u\n"
            "  attr_mask           0x%08x\n"
            "  aabb_min            (%g, %g, %g)\n"
            "  aabb_max            (%g, %g, %g)\n"
            "  source_hash         0x%08x\n"
            "  vertex_dedup_ratio  %.6f\n"
            "  tangent_fallback    %s\n",
            static_cast<unsigned long long>(stats.bytes_written),
            stats.source_vertex_count,
            stats.cooked_vertex_count,
            stats.index_count,
            stats.submesh_count,
            stats.attr_mask,
            static_cast<double>(stats.aabb_min[0]),
            static_cast<double>(stats.aabb_min[1]),
            static_cast<double>(stats.aabb_min[2]),
            static_cast<double>(stats.aabb_max[0]),
            static_cast<double>(stats.aabb_max[1]),
            static_cast<double>(stats.aabb_max[2]),
            stats.source_hash,
            stats.vertex_dedup_ratio,
            stats.tangent_fallback_used ? "yes" : "no");
    } else if (!opts.quiet) {
        std::fprintf(stdout,
            "psymesh_cook: wrote %llu bytes to '%s' "
            "(%u verts, %u indices, %u submeshes, attr_mask=0x%x)\n",
            static_cast<unsigned long long>(stats.bytes_written),
            opts.output_path.c_str(),
            stats.cooked_vertex_count,
            stats.index_count,
            stats.submesh_count,
            stats.attr_mask);
    }
    return 0;
}
