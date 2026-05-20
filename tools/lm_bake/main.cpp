// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 24 lm_bake CLI entrypoint.
//
// Builds a bake Scene from a Quake-style .map (brush faces become surfaces,
// `light` entities become point lights) and writes a baked .lmt lightmap.
// The core path tracer (LmBake.h) is map-agnostic; this shell is the .map
// front-end, reusing the shared parser in tools/lm_qbsp/MapSource.h.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

#include "../lm_qbsp/MapSource.h"
#include "LmBake.h"

namespace {

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "lm_bake - Psynder-GX offline path-traced lightmap baker (Lane 24)\n"
                 "\n"
                 "USAGE:\n"
                 "    lm_bake --in <MAP> --out <LMT> [OPTIONS]\n"
                 "\n"
                 "Bakes a static lightmap atlas (.lmt cooked-texture, RGBA8) from a\n"
                 "Quake-style .map: brush faces become lit surfaces and `light`\n"
                 "entities become point lights.\n"
                 "\n"
                 "OPTIONS:\n"
                 "    --in <PATH>            source .map\n"
                 "    --out <PATH>           output .lmt path\n"
                 "    --scale <FLOAT>        uniform scale applied to .map geometry + lights "
                 "(default 1.0)\n"
                 "    --texels-per-metre <F> lightmap density (default 4.0)\n"
                 "    --samples <N>          indirect hemisphere samples / texel (default 16)\n"
                 "    --bounces <N>          indirect bounce depth (default 1; 0 = direct only)\n"
                 "    --exposure <F>         linear exposure before tonemap (default 1.0)\n"
                 "    --ambient <F>          flat ambient sky term (default 0.0)\n"
                 "    --max-atlas <N>        atlas dimension cap (default 1024)\n"
                 "    --force                overwrite an existing output file\n"
                 "    --print-stats          dump atlas + lighting stats to stdout\n"
                 "    --quiet                suppress the final summary line\n"
                 "    -h, --help             show this message\n"
                 "\n"
                 "UNITS: 1 world unit = 1 metre; point-light falloff is inverse-square in metres.\n"
                 "DETERMINISM: identical input produces byte-identical .lmt output.\n");
}

psy::lm_bake::Vec3 to_bake(psy::lmtools::Vec3 v) {
    return psy::lm_bake::Vec3{v.x, v.y, v.z};
}

bool slurp_file(const std::string& path, std::vector<std::uint8_t>& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        err = "cannot open input file '" + path + "': " + std::strerror(errno);
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) {
        err = "cannot stat input file '" + path + "'";
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (!f) {
            err = "short read from '" + path + "'";
            return false;
        }
    }
    return true;
}

// Parse up to 3 whitespace-separated floats from a string ("0 0 64").
int parse_floats(std::string_view s, float* out, int max_count) {
    int n = 0;
    std::size_t i = 0;
    while (i < s.size() && n < max_count) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            ++i;
        }
        if (i > start) {
            float f = 0.0f;
            if (psy::lmtools::parse_float(s.substr(start, i - start), f)) {
                out[n++] = f;
            } else {
                break;
            }
        }
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    std::string in_path;
    psy::lm_bake::BakeOptions opts;
    float map_scale = 1.0f;  // uniform scale applied to .map geometry + lights

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next_arg = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lm_bake: error: %s requires an argument\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        auto next_float = [&](const char* name, float& dst) -> bool {
            const char* p = next_arg(name);
            if (!p)
                return false;
            // Locale-independent parse (matches the shared .map parser).
            if (!psy::lmtools::parse_float(p, dst)) {
                std::fprintf(stderr, "lm_bake: error: %s expects a float\n", name);
                return false;
            }
            return true;
        };
        auto next_u32 = [&](const char* name, std::uint32_t& dst) -> bool {
            const char* p = next_arg(name);
            if (!p)
                return false;
            char* end = nullptr;
            const long v = std::strtol(p, &end, 10);
            if (end == p || *end != '\0' || v < 0) {
                std::fprintf(stderr, "lm_bake: error: %s expects a non-negative integer\n", name);
                return false;
            }
            dst = static_cast<std::uint32_t>(v);
            return true;
        };

        if (a == "-h" || a == "--help") {
            print_usage(stdout);
            return 0;
        } else if (a == "--in") {
            const char* p = next_arg("--in");
            if (!p)
                return 2;
            in_path = p;
        } else if (a == "--out") {
            const char* p = next_arg("--out");
            if (!p)
                return 2;
            opts.output_path = p;
        } else if (a == "--scale") {
            if (!next_float("--scale", map_scale) || !(map_scale > 0.0f)) {
                std::fprintf(stderr, "lm_bake: error: --scale must be a positive float\n");
                return 2;
            }
        } else if (a == "--texels-per-metre") {
            if (!next_float("--texels-per-metre", opts.texels_per_metre))
                return 2;
        } else if (a == "--samples") {
            if (!next_u32("--samples", opts.samples))
                return 2;
        } else if (a == "--bounces") {
            if (!next_u32("--bounces", opts.bounces))
                return 2;
        } else if (a == "--exposure") {
            if (!next_float("--exposure", opts.exposure))
                return 2;
        } else if (a == "--ambient") {
            float amb = 0.0f;
            if (!next_float("--ambient", amb))
                return 2;
            opts.ambient = psy::lm_bake::Vec3{amb, amb, amb};
        } else if (a == "--max-atlas") {
            if (!next_u32("--max-atlas", opts.max_atlas_dim))
                return 2;
        } else if (a == "--force") {
            opts.force_overwrite = true;
        } else if (a == "--print-stats") {
            opts.print_stats = true;
        } else if (a == "--quiet") {
            opts.quiet = true;
        } else {
            std::fprintf(stderr, "lm_bake: error: unrecognised argument '%s'\n", argv[i]);
            print_usage(stderr);
            return 2;
        }
    }

    if (in_path.empty() || opts.output_path.empty()) {
        std::fprintf(stderr, "lm_bake: error: --in and --out are required\n");
        print_usage(stderr);
        return 2;
    }

    // ── Read + parse the .map ─────────────────────────────────────────────
    std::vector<std::uint8_t> raw;
    {
        std::string err;
        if (!slurp_file(in_path, raw, err)) {
            std::fprintf(stderr, "lm_bake: error: %s\n", err.c_str());
            return 1;
        }
    }
    psy::lmtools::MapFile map;
    psy::lmtools::MapParseError perr;
    if (!psy::lmtools::parse_map(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()),
                                 map,
                                 perr)) {
        std::fprintf(stderr,
                     "lm_bake: error: parse failed at line %zu: %s\n",
                     perr.line,
                     perr.message.c_str());
        return 1;
    }

    // ── Build the scene ───────────────────────────────────────────────────
    psy::lm_bake::Scene scene;
    scene.materials.push_back(psy::lm_bake::BakeMaterial{});  // material 0 = default
    const float extent = psy::lmtools::map_world_extent(map);

    for (const psy::lmtools::MapEntity& ent : map.entities) {
        for (const psy::lmtools::MapBrush& brush : ent.brushes) {
            const std::vector<psy::lmtools::FacePolygon> polys =
                psy::lmtools::brush_build_polygons(brush, extent);
            for (const psy::lmtools::FacePolygon& fp : polys) {
                psy::lm_bake::BakeSurface surf;
                surf.normal = to_bake(fp.plane.n);
                surf.material = 0;
                surf.vertices.reserve(fp.vertices.size());
                for (psy::lmtools::Vec3 v : fp.vertices) {
                    surf.vertices.push_back(to_bake(v) * map_scale);
                }
                scene.surfaces.push_back(std::move(surf));
            }
        }
        if (ent.classname() == "light") {
            float origin[3] = {0.0f, 0.0f, 0.0f};
            parse_floats(ent.get("origin"), origin, 3);
            psy::lm_bake::BakeLight light;
            light.kind = psy::lm_bake::BakeLight::Kind::Point;
            light.position = psy::lm_bake::Vec3{origin[0], origin[1], origin[2]} * map_scale;
            float intensity = 300.0f;
            const std::string_view ls = ent.get("light");
            if (!ls.empty()) {
                float tmp = 0.0f;
                if (parse_floats(ls, &tmp, 1) == 1) {
                    intensity = tmp;
                }
            }
            light.intensity = intensity;
            float color[3] = {1.0f, 1.0f, 1.0f};
            const std::string_view cs = ent.get("_color");
            if (parse_floats(cs, color, 3) == 3) {
                if (color[0] > 1.0f || color[1] > 1.0f || color[2] > 1.0f) {
                    color[0] /= 255.0f;
                    color[1] /= 255.0f;
                    color[2] /= 255.0f;
                }
                light.color = psy::lm_bake::Vec3{color[0], color[1], color[2]};
            }
            scene.lights.push_back(light);
        }
    }

    psy::lm_bake::BakeStats stats;
    std::string err;
    if (!psy::lm_bake::bake(scene, opts, &stats, nullptr, &err)) {
        std::fprintf(stderr, "lm_bake: error: %s\n", err.c_str());
        return 1;
    }

    if (opts.print_stats) {
        std::fprintf(stdout,
                     "lm_bake stats:\n"
                     "  bytes_written   %llu\n"
                     "  surfaces        %u\n"
                     "  triangles       %u\n"
                     "  lights          %u\n"
                     "  atlas           %u x %u\n"
                     "  texels_lit      %u\n"
                     "  max_luminance   %g\n",
                     static_cast<unsigned long long>(stats.bytes_written),
                     stats.surface_count,
                     stats.triangle_count,
                     stats.light_count,
                     stats.atlas_width,
                     stats.atlas_height,
                     stats.texels_lit,
                     stats.max_luminance);
    } else if (!opts.quiet) {
        std::fprintf(stdout,
                     "lm_bake: wrote %llu bytes to '%s' (%u surfaces, %u lights, atlas %u x %u)\n",
                     static_cast<unsigned long long>(stats.bytes_written),
                     opts.output_path.c_str(),
                     stats.surface_count,
                     stats.light_count,
                     stats.atlas_width,
                     stats.atlas_height);
    }
    return 0;
}
