// SPDX-License-Identifier: MIT
// Psynder-GX — decode web-console opcodes and run them on the engine console.

#include "ConsoleBridge.h"

#include "Ipc.h"
#include "core/console/Completion.h"
#include "core/console/Console.h"
#include "script/Script.h"
#include "script/internal/VisualGraphCompiler.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <sstream>
#include <utility>
#include <vector>

namespace psynder::editor::ipc {

namespace detail {
void set_on_message(std::function<void(std::span<const u8>)> cb);
}  // namespace detail

namespace {

enum Opcode : u64 {
    kSubscribe = 3,
    kUnsubscribe = 4,
    kStatsFrame = 18,
    kConsole = 19,
    kConsoleReply = 21,
    kConsoleCompletionQuery = 22,
    kConsoleCompletionReply = 23,
    kPsyGraphCompile = 30,
    kPsyGraphCompileReply = 31,
    kPsyGraphSave = 32,
    kPsyGraphSaveReply = 33,
    kSceneCommand = 40,
    kSceneCommandReply = 41,
};

struct MsgReader {
    const u8* data = nullptr;
    usize size = 0;
    usize pos = 0;

    explicit MsgReader(std::span<const u8> bytes)
        : data(bytes.data()), size(bytes.size()) {}

    bool read_u8(u8& out) {
        if (pos >= size) return false;
        out = data[pos++];
        return true;
    }

    bool read_be(u64& out, usize n) {
        if (n > 8 || pos + n > size) return false;
        out = 0;
        for (usize i = 0; i < n; ++i) {
            out = (out << 8) | data[pos++];
        }
        return true;
    }

    bool read_uint(u64& out) {
        u8 tag = 0;
        if (!read_u8(tag)) return false;
        if (tag <= 0x7f) {
            out = tag;
            return true;
        }
        if (tag == 0xcc) return read_be(out, 1);
        if (tag == 0xcd) return read_be(out, 2);
        if (tag == 0xce) return read_be(out, 4);
        if (tag == 0xcf) return read_be(out, 8);
        if (tag == 0xd0) {
            u64 raw = 0;
            if (!read_be(raw, 1)) return false;
            out = static_cast<u8>(raw);
            return true;
        }
        if (tag == 0xd1 || tag == 0xd2 || tag == 0xd3) {
            return false;
        }
        return false;
    }

    bool read_array(usize& out_count) {
        u8 tag = 0;
        if (!read_u8(tag)) return false;
        if ((tag & 0xf0) == 0x90) {
            out_count = tag & 0x0f;
            return true;
        }
        if (tag == 0xdc) {
            u64 n = 0;
            if (!read_be(n, 2)) return false;
            out_count = static_cast<usize>(n);
            return true;
        }
        if (tag == 0xdd) {
            u64 n = 0;
            if (!read_be(n, 4)) return false;
            out_count = static_cast<usize>(n);
            return true;
        }
        return false;
    }

    bool read_string(std::string& out) {
        u8 tag = 0;
        if (!read_u8(tag)) return false;

        u64 n = 0;
        if ((tag & 0xe0) == 0xa0) {
            n = tag & 0x1f;
        } else if (tag == 0xd9) {
            if (!read_be(n, 1)) return false;
        } else if (tag == 0xda) {
            if (!read_be(n, 2)) return false;
        } else if (tag == 0xdb) {
            if (!read_be(n, 4)) return false;
        } else {
            return false;
        }
        if (pos + n > size) return false;
        out.assign(reinterpret_cast<const char*>(data + pos),
                   static_cast<usize>(n));
        pos += static_cast<usize>(n);
        return true;
    }
};

void pack_uint(std::vector<u8>& out, u64 value) {
    if (value <= 0x7f) {
        out.push_back(static_cast<u8>(value));
    } else if (value <= 0xff) {
        out.push_back(0xcc);
        out.push_back(static_cast<u8>(value));
    } else if (value <= 0xffff) {
        out.push_back(0xcd);
        out.push_back(static_cast<u8>(value >> 8));
        out.push_back(static_cast<u8>(value));
    } else if (value <= 0xffffffffull) {
        out.push_back(0xce);
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<u8>(value >> shift));
        }
    } else {
        out.push_back(0xcf);
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<u8>(value >> shift));
        }
    }
}

void pack_bool(std::vector<u8>& out, bool value) {
    out.push_back(value ? 0xc3 : 0xc2);
}

void pack_f64(std::vector<u8>& out, double value) {
    u64 bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double pack assumes IEEE 754 f64");
    std::memcpy(&bits, &value, sizeof(bits));
    out.push_back(0xcb);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<u8>(bits >> shift));
    }
}

void pack_array(std::vector<u8>& out, usize count) {
    if (count <= 15) {
        out.push_back(static_cast<u8>(0x90 | count));
    } else {
        out.push_back(0xdc);
        out.push_back(static_cast<u8>(count >> 8));
        out.push_back(static_cast<u8>(count));
    }
}

void pack_map(std::vector<u8>& out, usize count) {
    if (count <= 15) {
        out.push_back(static_cast<u8>(0x80 | count));
    } else {
        out.push_back(0xde);
        out.push_back(static_cast<u8>(count >> 8));
        out.push_back(static_cast<u8>(count));
    }
}

void pack_string(std::vector<u8>& out, std::string_view text) {
    const usize n = text.size();
    if (n <= 31) {
        out.push_back(static_cast<u8>(0xa0 | n));
    } else if (n <= 0xff) {
        out.push_back(0xd9);
        out.push_back(static_cast<u8>(n));
    } else if (n <= 0xffff) {
        out.push_back(0xda);
        out.push_back(static_cast<u8>(n >> 8));
        out.push_back(static_cast<u8>(n));
    } else {
        out.push_back(0xdb);
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<u8>(n >> shift));
        }
    }
    out.insert(out.end(),
               reinterpret_cast<const u8*>(text.data()),
               reinterpret_cast<const u8*>(text.data()) + text.size());
}

std::vector<u8> begin_opcode_frame(u64 opcode, usize body_items) {
    std::vector<u8> out;
    out.reserve(128);
    pack_uint(out, opcode);
    pack_array(out, body_items);
    return out;
}

u64 completion_kind(console::CompletionKind kind) {
    switch (kind) {
        case console::CompletionKind::Command: return 1;
        case console::CompletionKind::Value: return 2;
        case console::CompletionKind::Cvar:
        default: return 0;
    }
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

bool is_script_repl_line(std::string_view line) {
    return starts_with(line, ":graph") ||
           starts_with(line, ":vscript") ||
           starts_with(line, ":visual");
}

bool safe_project_relative_path(std::string_view text, std::filesystem::path& out) {
    if (text.empty()) {
        return false;
    }
    std::filesystem::path path{text};
    if (path.is_absolute()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }

    // Lexical "no .." is not enough: a symlink already living inside the
    // project can redirect a write/read outside it. Resolve the path (which
    // follows symlinks on the existing prefix) and confirm it stays under the
    // project root before handing it back. The IPC peer is a local websocket,
    // so this is the trust boundary for filesystem access.
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::current_path(ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path base_real = std::filesystem::weakly_canonical(base, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(base_real / path, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path rel = resolved.lexically_relative(base_real);
    if (rel.empty() || rel.begin()->native() == "..") {
        return false;
    }

    out = std::move(path);
    return true;
}

bool write_text_file(std::string_view path_text, std::string_view text, std::string& out_path, std::string& err) {
    std::filesystem::path rel;
    if (!safe_project_relative_path(path_text, rel)) {
        err = "save rejected unsafe path";
        return false;
    }

    std::error_code ec;
    if (rel.has_parent_path()) {
        std::filesystem::create_directories(rel.parent_path(), ec);
        if (ec) {
            err = "save mkdir failed: ";
            err += ec.message();
            return false;
        }
    }

    std::ofstream file(rel, std::ios::binary | std::ios::trunc);
    if (!file) {
        err = "save open failed";
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) {
        err = "save write failed";
        return false;
    }
    out_path = rel.generic_string();
    return true;
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

std::string json_string_field(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string_view::npos) return {};
    std::string out;
    bool esc = false;
    for (std::size_t i = pos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (esc) {
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else if (c == 't') out.push_back('\t');
            else out.push_back(c);
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

struct SceneAuthoringState {
    std::string path = "projects/PsyArcadeGX/scenes/startup.scene.bin";
    std::string name = "Startup";
    bool has_scene = false;
    bool has_camera = false;
    bool dirty = false;
    std::string document_json;
};

SceneAuthoringState& scene_state() {
    static SceneAuthoringState s;
    return s;
}

SceneDocumentChangedFn& scene_document_changed_callback() {
    static SceneDocumentChangedFn cb;
    return cb;
}

void notify_scene_document_changed() {
    SceneAuthoringState& s = scene_state();
    SceneDocumentChangedFn& cb = scene_document_changed_callback();
    if (cb && s.has_scene) {
        cb(s.path, s.document_json);
    }
}

std::string make_default_scene_json(std::string_view name,
                                    std::string_view path,
                                    bool has_camera) {
    std::ostringstream out;
    out << "{\n"
        << "  \"format\": \"psynder-gx-loose-scene-v1\",\n"
        << "  \"name\": \"" << json_escape(name) << "\",\n"
        << "  \"path\": \"" << json_escape(path) << "\",\n"
        << "  \"active_camera\": "
        << (has_camera ? "\"Camera\"" : "null") << ",\n"
        << "  \"environmentSettings\": {\n"
        << "    \"clear_color\": [0.018, 0.027, 0.050, 1.0],\n"
        << "    \"sky_mode\": \"sdf-boot-field\"\n"
        << "  },\n"
        << "  \"entities\": [\n";
    if (has_camera) {
        out << "    {\n"
            << "      \"name\": \"Camera\",\n"
            << "      \"components\": [\"Transform\", \"Camera\"],\n"
            << "      \"position\": [0.0, 1.4, 4.2],\n"
            << "      \"rotation_euler_deg\": [-8.0, 180.0, 0.0],\n"
            << "      \"fov_y_deg\": 70.0\n"
            << "    }\n";
    }
    out << "  ]\n"
        << "}\n";
    return out.str();
}

bool read_text_file(std::string_view path_text, std::string& out, std::string& err) {
    std::filesystem::path rel;
    if (!safe_project_relative_path(path_text, rel)) {
        err = "scene load rejected unsafe path";
        return false;
    }
    std::ifstream file(rel, std::ios::binary);
    if (!file) {
        err = "scene load open failed";
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    if (!file.good() && !file.eof()) {
        err = "scene load read failed";
        return false;
    }
    return true;
}

bool update_project_manifest_startup_scene(std::string_view scene_path, std::string& err) {
    constexpr std::string_view kProjectRoot = "projects/PsyArcadeGX/";
    std::string path{scene_path};
    std::string vfs_path;
    if (starts_with(path, kProjectRoot)) {
        vfs_path = "game:/";
        vfs_path += path.substr(kProjectRoot.size());
    } else {
        vfs_path = path;
    }

    const std::string manifest_path = "projects/PsyArcadeGX/manifest.psycooked";
    std::filesystem::create_directories("projects/PsyArcadeGX");
    std::ofstream file(manifest_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        err = "manifest write failed";
        return false;
    }
    file << "# Psynder-GX loose project manifest\n"
         << "startup_scene = \"" << vfs_path << "\"\n";
    if (!file) {
        err = "manifest write failed";
        return false;
    }
    return true;
}

void sync_scene_state_from_document(SceneAuthoringState& s) {
    const std::string name = json_string_field(s.document_json, "name");
    const std::string path = json_string_field(s.document_json, "path");
    if (!name.empty()) s.name = name;
    if (!path.empty()) s.path = path;
    s.has_camera = s.document_json.find("\"Camera\"") != std::string::npos ||
                   s.document_json.find("\"active_camera\"") != std::string::npos;
    if (s.document_json.find("\"active_camera\": null") != std::string::npos) {
        s.has_camera = false;
    }
}

std::size_t scene_entity_count(std::string_view document_json) {
    std::size_t count = 0;
    std::size_t cursor = 0;
    while (true) {
        const std::size_t pos = document_json.find("\"components\"", cursor);
        if (pos == std::string_view::npos) {
            return count;
        }
        ++count;
        cursor = pos + 12;
    }
}

std::string scene_status_json(std::string_view message) {
    const SceneAuthoringState& s = scene_state();
    const std::size_t entity_count = s.has_scene
        ? scene_entity_count(s.document_json)
        : 0;
    std::ostringstream out;
    out << "{"
        << "\"name\":\"" << json_escape(s.name) << "\","
        << "\"path\":\"" << json_escape(s.path) << "\","
        << "\"has_scene\":" << (s.has_scene ? "true" : "false") << ","
        << "\"has_camera\":" << (s.has_camera ? "true" : "false") << ","
        << "\"dirty\":" << (s.dirty ? "true" : "false") << ","
        << "\"entity_count\":" << entity_count << ","
        << "\"message\":\"" << json_escape(message) << "\","
        << "\"document_json\":\"" << json_escape(s.document_json) << "\""
        << "}";
    return out.str();
}

void broadcast_console_reply(bool ok, std::string_view text) {
    std::vector<u8> frame = begin_opcode_frame(kConsoleReply, 2);
    pack_bool(frame, ok);
    pack_string(frame, text);
    Server::Get().broadcast("console", frame);
}

void broadcast_psygraph_compile_reply(u64 id,
                                      bool ok,
                                      std::string_view text,
                                      std::string_view generated_source) {
    std::vector<u8> frame = begin_opcode_frame(kPsyGraphCompileReply, 4);
    pack_uint(frame, id);
    pack_bool(frame, ok);
    pack_string(frame, text);
    pack_string(frame, generated_source);
    Server::Get().broadcast("psygraph", frame);
}

void broadcast_psygraph_save_reply(u64 id,
                                   bool ok,
                                   std::string_view text,
                                   std::string_view path) {
    std::vector<u8> frame = begin_opcode_frame(kPsyGraphSaveReply, 4);
    pack_uint(frame, id);
    pack_bool(frame, ok);
    pack_string(frame, text);
    pack_string(frame, path);
    Server::Get().broadcast("psygraph", frame);
}

void broadcast_scene_command_reply(u64 id,
                                   bool ok,
                                   std::string_view text,
                                   std::string_view path,
                                   std::string_view document_json) {
    std::vector<u8> frame = begin_opcode_frame(kSceneCommandReply, 5);
    pack_uint(frame, id);
    pack_bool(frame, ok);
    pack_string(frame, text);
    pack_string(frame, path);
    pack_string(frame, document_json);
    Server::Get().broadcast("scene", frame);
}

void broadcast_completions(u64 id,
                           const console::TokenInfo& token,
                           const std::vector<console::CompletionMatch>& matches) {
    std::vector<u8> frame = begin_opcode_frame(kConsoleCompletionReply, 7);
    pack_uint(frame, id);
    pack_uint(frame, token.start);
    pack_uint(frame, token.end);

    pack_array(frame, matches.size());
    for (const auto& m : matches) pack_string(frame, m.name);

    pack_array(frame, matches.size());
    for (const auto& m : matches) pack_uint(frame, completion_kind(m.kind));

    pack_array(frame, matches.size());
    for (const auto& m : matches) pack_string(frame, m.value);

    pack_array(frame, matches.size());
    for (const auto& m : matches) pack_string(frame, m.description);

    Server::Get().broadcast("console", frame);
}

void pack_profiler_sections(std::vector<u8>& frame,
                            std::span<const ProfilerSectionSample> sections) {
    pack_array(frame, sections.size());
    for (const ProfilerSectionSample& section : sections) {
        pack_map(frame, 2);
        pack_string(frame, "name");
        pack_string(frame, section.name);
        pack_string(frame, "ms");
        pack_f64(frame, section.ms);
    }
}

void handle_psygraph_compile(MsgReader& r) {
    usize count = 0;
    u64 id = 0;
    std::string graph_json;
    std::string source_path;
    if (!r.read_array(count) || count < 2 ||
        !r.read_uint(id) || !r.read_string(graph_json)) {
        broadcast_psygraph_compile_reply(0, false, "editor-ipc: malformed psygraph compile frame", "");
        return;
    }
    if (count >= 3) {
        (void)r.read_string(source_path);
    }

    const script::detail::VisualCompileResult compiled =
        script::detail::compile_visual_graph(graph_json);
    if (!compiled.ok) {
        broadcast_psygraph_compile_reply(id, false, compiled.diagnostic, "");
        return;
    }

    std::string repl_out;
    const bool ran = script::Vm::Get().execute_repl(compiled.lua_source, repl_out);
    if (!ran) {
        broadcast_psygraph_compile_reply(id, false, repl_out, compiled.lua_source);
        return;
    }

    std::string text = "compiled";
    if (!source_path.empty()) {
        text += " ";
        text += source_path;
    }
    if (!repl_out.empty()) {
        text += "\n";
        text += repl_out;
    }
    std::string generated_source;
    generated_source.reserve(compiled.lua_source.size() +
                             compiled.psyscript_source.size() +
                             compiled.cpp_source.size() + 96);
    generated_source += "-- Lua preview\n";
    generated_source += compiled.lua_source;
    generated_source += "\n# PsyScript IR\n";
    generated_source += compiled.psyscript_source;
    generated_source += "\n// Native C++\n";
    generated_source += compiled.cpp_source;
    broadcast_psygraph_compile_reply(id, true, text, generated_source);
}

void handle_psygraph_save(MsgReader& r) {
    usize count = 0;
    u64 id = 0;
    std::string source_path;
    std::string document_json;
    if (!r.read_array(count) || count < 3 ||
        !r.read_uint(id) || !r.read_string(source_path) || !r.read_string(document_json)) {
        broadcast_psygraph_save_reply(0, false, "editor-ipc: malformed psygraph save frame", "");
        return;
    }

    std::string written_path;
    std::string err;
    if (!write_text_file(source_path, document_json, written_path, err)) {
        broadcast_psygraph_save_reply(id, false, err, source_path);
        return;
    }
    broadcast_psygraph_save_reply(id, true, "saved", written_path);
}

void handle_scene_command(MsgReader& r) {
    usize count = 0;
    u64 id = 0;
    std::string action;
    std::string path;
    std::string name;
    std::string document_json;
    if (!r.read_array(count) || count < 2 ||
        !r.read_uint(id) || !r.read_string(action)) {
        broadcast_scene_command_reply(0,
                                      false,
                                      "editor-ipc: malformed scene command frame",
                                      "",
                                      "{}");
        return;
    }
    if (count >= 3) (void)r.read_string(path);
    if (count >= 4) (void)r.read_string(name);
    if (count >= 5) (void)r.read_string(document_json);

    SceneAuthoringState& s = scene_state();
    if (action == "status") {
        broadcast_scene_command_reply(id,
                                      true,
                                      s.has_scene ? "scene open" : "no scene open",
                                      s.path,
                                      scene_status_json("status"));
        return;
    }

    if (action == "create") {
        if (path.empty()) path = "projects/PsyArcadeGX/scenes/startup.scene.bin";
        if (name.empty()) name = "Startup";
        std::filesystem::path rel;
        if (!safe_project_relative_path(path, rel)) {
            broadcast_scene_command_reply(id, false, "scene create rejected unsafe path", path, "{}");
            return;
        }
        s.path = rel.generic_string();
        s.name = name;
        s.has_scene = true;
        s.has_camera = true;
        s.dirty = true;
        s.document_json = make_default_scene_json(s.name, s.path, s.has_camera);
        notify_scene_document_changed();
        broadcast_scene_command_reply(id, true, "created scene", s.path, scene_status_json("created scene"));
        return;
    }

    if (action == "load") {
        if (path.empty()) path = s.path;
        std::string text;
        std::string err;
        if (!read_text_file(path, text, err)) {
            broadcast_scene_command_reply(id, false, err, path, scene_status_json(err));
            return;
        }
        s.path = path;
        s.document_json = std::move(text);
        s.name = json_string_field(s.document_json, "name");
        if (s.name.empty()) s.name = std::filesystem::path{s.path}.stem().generic_string();
        s.has_scene = true;
        s.dirty = false;
        sync_scene_state_from_document(s);
        notify_scene_document_changed();
        broadcast_scene_command_reply(id, true, "loaded scene", s.path, scene_status_json("loaded scene"));
        return;
    }

    if (action == "save") {
        if (!path.empty()) s.path = path;
        if (!name.empty()) s.name = name;
        if (!document_json.empty()) {
            s.document_json = document_json;
            sync_scene_state_from_document(s);
            if (!path.empty()) s.path = path;
            if (!name.empty()) s.name = name;
        }
        if (s.document_json.empty()) {
            s.document_json = make_default_scene_json(s.name, s.path, true);
            s.has_camera = true;
        }

        std::string written_path;
        std::string err;
        if (!write_text_file(s.path, s.document_json, written_path, err)) {
            broadcast_scene_command_reply(id, false, err, s.path, scene_status_json(err));
            return;
        }
        s.path = written_path;
        s.has_scene = true;
        s.dirty = false;
        notify_scene_document_changed();
        std::string manifest_err;
        const bool manifest_ok = update_project_manifest_startup_scene(s.path, manifest_err);
        const std::string message = manifest_ok
            ? "saved scene and updated startup manifest"
            : "saved scene; " + manifest_err;
        broadcast_scene_command_reply(id, manifest_ok, message, s.path, scene_status_json(message));
        return;
    }

    if (action == "sync") {
        if (!path.empty()) s.path = path;
        if (!name.empty()) s.name = name;
        if (!document_json.empty()) {
            s.document_json = document_json;
            sync_scene_state_from_document(s);
            if (!path.empty()) s.path = path;
            if (!name.empty()) s.name = name;
        }
        if (s.document_json.empty()) {
            s.document_json = make_default_scene_json(s.name, s.path, true);
            s.has_camera = true;
        }
        s.has_scene = true;
        s.dirty = true;
        notify_scene_document_changed();
        broadcast_scene_command_reply(id, true, "synced scene", s.path, scene_status_json("synced scene"));
        return;
    }

    broadcast_scene_command_reply(id, false, "unknown scene action", s.path, scene_status_json("unknown scene action"));
}

void handle_console(MsgReader& r) {
    usize count = 0;
    std::string text;
    std::string mode;
    if (!r.read_array(count) || count < 1 || !r.read_string(text)) {
        broadcast_console_reply(false, "editor-ipc: malformed console frame");
        return;
    }
    if (count >= 2) {
        (void)r.read_string(mode);
    }
    std::string line = text;
    if (mode == "lua") {
        line = "lua " + line;
    }
    if (mode != "lua" && is_script_repl_line(line)) {
        std::string reply;
        const bool ok = script::Vm::Get().execute_repl(line, reply);
        broadcast_console_reply(ok, reply);
        return;
    }
    auto result = console::Console::Get().Execute(line);
    std::string reply = result.output;
    if (!result.error.empty()) {
        if (!reply.empty() && reply.back() != '\n') reply.push_back('\n');
        reply += result.error;
    }
    broadcast_console_reply(result.ok, reply);
}

void handle_completion(MsgReader& r) {
    usize count = 0;
    u64 id = 0;
    std::string input;
    u64 cursor = 0;
    if (!r.read_array(count) || count < 3 ||
        !r.read_uint(id) || !r.read_string(input) || !r.read_uint(cursor)) {
        broadcast_console_reply(false, "editor-ipc: malformed completion frame");
        return;
    }
    const auto token = console::CurrentToken(input, static_cast<usize>(cursor));
    auto matches = console::BuildCompletions(token, 40, 120);
    broadcast_completions(id, token, matches);
}

void handle_message(std::span<const u8> bytes) {
    MsgReader r(bytes);
    u64 op = 0;
    if (!r.read_uint(op)) {
        return;
    }
    switch (op) {
        case kConsole:
            handle_console(r);
            break;
        case kConsoleCompletionQuery:
            handle_completion(r);
            break;
        case kPsyGraphCompile:
            handle_psygraph_compile(r);
            break;
        case kPsyGraphSave:
            handle_psygraph_save(r);
            break;
        case kSceneCommand:
            handle_scene_command(r);
            break;
        case kSubscribe:
        case kUnsubscribe:
            break;
        default:
            break;
    }
}

}  // namespace

void install_console_bridge() {
    detail::set_on_message([](std::span<const u8> bytes) {
        handle_message(bytes);
    });
}

void set_scene_document_changed_callback(SceneDocumentChangedFn cb) {
    scene_document_changed_callback() = std::move(cb);
}

void seed_scene_document(std::string_view path,
                         std::string_view name,
                         std::string_view document_json) {
    if (document_json.empty()) {
        return;
    }
    SceneAuthoringState& s = scene_state();
    s.path = path.empty() ? "projects/PsyArcadeGX/scenes/startup.scene.bin" : std::string{path};
    s.name = name.empty() ? "Startup" : std::string{name};
    s.document_json = std::string{document_json};
    s.has_scene = true;
    s.dirty = false;
    sync_scene_state_from_document(s);
}

void publish_profiler_frame(std::uint64_t frame_index,
                            double cpu_ms,
                            double gpu_ms,
                            std::uint32_t draw_calls,
                            std::uint32_t entities,
                            std::span<const ProfilerSectionSample> sections) {
    std::vector<u8> frame;
    frame.reserve(256);
    pack_uint(frame, kStatsFrame);
    pack_map(frame, 6);
    pack_string(frame, "frame");
    pack_uint(frame, frame_index);
    pack_string(frame, "cpu_ms");
    pack_f64(frame, cpu_ms);
    pack_string(frame, "gpu_ms");
    pack_f64(frame, gpu_ms);
    pack_string(frame, "draw_calls");
    pack_uint(frame, draw_calls);
    pack_string(frame, "entities");
    pack_uint(frame, entities);
    pack_string(frame, "sections");
    pack_profiler_sections(frame, sections);
    Server::Get().broadcast("stats", frame);
}

void remove_console_bridge() {
    scene_document_changed_callback() = {};
    detail::set_on_message({});
}

}  // namespace psynder::editor::ipc
