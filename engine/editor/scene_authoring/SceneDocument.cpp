// SPDX-License-Identifier: MIT OR Apache-2.0

#include "SceneDocument.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

namespace psynder::editor::scene_authoring {
namespace {

enum class JsonKind {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

struct JsonValue;

struct JsonMember {
    std::string key;
    std::size_t value_index = 0;
};

struct JsonValue {
    JsonKind kind = JsonKind::Null;
    bool bool_value = false;
    double number_value = 0.0;
    std::string string_value;
    std::vector<JsonValue> array_values;
    std::vector<JsonMember> object_values;
};

struct Parser {
    std::string_view text;
    std::size_t cursor = 0;
    ParseDiagnostics* diag = nullptr;

    void set_error(std::string message) const {
        if (diag != nullptr && diag->ok) {
            diag->ok = false;
            diag->offset = cursor;
            diag->message = std::move(message);
        }
    }

    void skip_ws() noexcept {
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        skip_ws();
        if (cursor < text.size() && text[cursor] == expected) {
            ++cursor;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parse_string(std::string& out) {
        skip_ws();
        if (cursor >= text.size() || text[cursor] != '"') {
            set_error("expected string");
            return false;
        }
        ++cursor;
        out.clear();
        while (cursor < text.size()) {
            const char c = text[cursor++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (cursor >= text.size()) {
                set_error("unterminated escape");
                return false;
            }
            const char e = text[cursor++];
            switch (e) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    // Loose scene ids are ASCII today. Preserve unknown Unicode
                    // escapes as a stable literal instead of failing authoring.
                    out.append("\\u");
                    for (int i = 0; i < 4 && cursor < text.size(); ++i) {
                        out.push_back(text[cursor++]);
                    }
                    break;
                default:
                    out.push_back(e);
                    break;
            }
        }
        set_error("unterminated string");
        return false;
    }

    [[nodiscard]] bool parse_number(JsonValue& out) {
        skip_ws();
        const std::size_t begin = cursor;
        if (cursor < text.size() && text[cursor] == '-') {
            ++cursor;
        }
        while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor < text.size() && text[cursor] == '.') {
            ++cursor;
            while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
                ++cursor;
            }
        }
        if (cursor < text.size() && (text[cursor] == 'e' || text[cursor] == 'E')) {
            ++cursor;
            if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-')) {
                ++cursor;
            }
            while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
                ++cursor;
            }
        }
        if (begin == cursor) {
            set_error("expected number");
            return false;
        }
        const std::string scratch{text.substr(begin, cursor - begin)};
        char* end = nullptr;
        const double value = std::strtod(scratch.c_str(), &end);
        if (end == scratch.c_str()) {
            set_error("invalid number");
            return false;
        }
        out.kind = JsonKind::Number;
        out.number_value = value;
        return true;
    }

    [[nodiscard]] bool parse_array(JsonValue& out) {
        if (!consume('[')) {
            set_error("expected array");
            return false;
        }
        out.kind = JsonKind::Array;
        out.array_values.clear();
        skip_ws();
        if (consume(']')) {
            return true;
        }
        for (;;) {
            JsonValue value{};
            if (!parse_value(value)) {
                return false;
            }
            out.array_values.push_back(std::move(value));
            skip_ws();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                set_error("expected array comma");
                return false;
            }
        }
    }

    [[nodiscard]] bool parse_object(JsonValue& out) {
        if (!consume('{')) {
            set_error("expected object");
            return false;
        }
        out.kind = JsonKind::Object;
        out.object_values.clear();
        skip_ws();
        if (consume('}')) {
            return true;
        }
        for (;;) {
            JsonMember member{};
            if (!parse_string(member.key)) {
                return false;
            }
            if (!consume(':')) {
                set_error("expected object colon");
                return false;
            }
            JsonValue value{};
            if (!parse_value(value)) {
                return false;
            }
            out.array_values.push_back(std::move(value));
            member.value_index = out.array_values.size() - 1u;
            out.object_values.push_back(std::move(member));
            skip_ws();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                set_error("expected object comma");
                return false;
            }
        }
    }

    [[nodiscard]] bool parse_literal(std::string_view literal, JsonKind kind, JsonValue& out) {
        skip_ws();
        if (text.substr(cursor, literal.size()) != literal) {
            set_error("expected literal");
            return false;
        }
        cursor += literal.size();
        out.kind = kind;
        if (literal == "true") {
            out.bool_value = true;
        } else if (literal == "false") {
            out.bool_value = false;
        }
        return true;
    }

    [[nodiscard]] bool parse_value(JsonValue& out) {
        skip_ws();
        if (cursor >= text.size()) {
            set_error("expected value");
            return false;
        }
        switch (text[cursor]) {
            case '{':
                return parse_object(out);
            case '[':
                return parse_array(out);
            case '"':
                out.kind = JsonKind::String;
                return parse_string(out.string_value);
            case 't':
                return parse_literal("true", JsonKind::Bool, out);
            case 'f':
                return parse_literal("false", JsonKind::Bool, out);
            case 'n':
                return parse_literal("null", JsonKind::Null, out);
            default:
                return parse_number(out);
        }
    }
};

[[nodiscard]] const JsonValue* member(const JsonValue& object, std::string_view key) noexcept {
    if (object.kind != JsonKind::Object) {
        return nullptr;
    }
    for (const JsonMember& item : object.object_values) {
        if (item.key == key) {
            if (item.value_index < object.array_values.size()) {
                return &object.array_values[item.value_index];
            }
            return nullptr;
        }
    }
    return nullptr;
}

[[nodiscard]] JsonValue* mutable_member(JsonValue& object, std::string_view key) noexcept {
    if (object.kind != JsonKind::Object) {
        return nullptr;
    }
    for (const JsonMember& item : object.object_values) {
        if (item.key == key) {
            if (item.value_index < object.array_values.size()) {
                return &object.array_values[item.value_index];
            }
            return nullptr;
        }
    }
    return nullptr;
}

[[nodiscard]] JsonValue make_bool(bool value) noexcept {
    JsonValue out{};
    out.kind = JsonKind::Bool;
    out.bool_value = value;
    return out;
}

[[nodiscard]] JsonValue make_number(double value) noexcept {
    JsonValue out{};
    out.kind = JsonKind::Number;
    out.number_value = value;
    return out;
}

[[nodiscard]] JsonValue make_string(std::string_view value) {
    JsonValue out{};
    out.kind = JsonKind::String;
    out.string_value.assign(value.data(), value.size());
    return out;
}

[[nodiscard]] JsonValue make_array() noexcept {
    JsonValue out{};
    out.kind = JsonKind::Array;
    return out;
}

[[nodiscard]] JsonValue make_object() noexcept {
    JsonValue out{};
    out.kind = JsonKind::Object;
    return out;
}

void set_object_member(JsonValue& object, std::string_view key, JsonValue value) {
    if (object.kind != JsonKind::Object) {
        object = make_object();
    }
    if (JsonValue* existing = mutable_member(object, key)) {
        *existing = std::move(value);
        return;
    }
    object.array_values.push_back(std::move(value));
    object.object_values.push_back(JsonMember{std::string{key}, object.array_values.size() - 1u});
}

JsonValue& ensure_object_member(JsonValue& object, std::string_view key) {
    if (object.kind != JsonKind::Object) {
        object = make_object();
    }
    if (JsonValue* existing = mutable_member(object, key)) {
        if (existing->kind != JsonKind::Object) {
            *existing = make_object();
        }
        return *existing;
    }
    object.array_values.push_back(make_object());
    object.object_values.push_back(JsonMember{std::string{key}, object.array_values.size() - 1u});
    return object.array_values.back();
}

[[nodiscard]] std::string string_or(const JsonValue& object, std::string_view key, std::string fallback) {
    const JsonValue* value = member(object, key);
    if (value == nullptr || value->kind != JsonKind::String) {
        return fallback;
    }
    return value->string_value;
}

[[nodiscard]] bool bool_or(const JsonValue& object, std::string_view key, bool fallback) noexcept {
    const JsonValue* value = member(object, key);
    if (value == nullptr || value->kind != JsonKind::Bool) {
        return fallback;
    }
    return value->bool_value;
}

[[nodiscard]] float float_or(const JsonValue& object, std::string_view key, float fallback) noexcept {
    const JsonValue* value = member(object, key);
    if (value == nullptr || value->kind != JsonKind::Number) {
        return fallback;
    }
    return static_cast<float>(value->number_value);
}

[[nodiscard]] int int_or(const JsonValue& object, std::string_view key, int fallback) noexcept {
    const JsonValue* value = member(object, key);
    if (value == nullptr || value->kind != JsonKind::Number) {
        return fallback;
    }
    return static_cast<int>(value->number_value);
}

[[nodiscard]] math::Vec3 vec3_or(const JsonValue& object,
                                 std::string_view key,
                                 math::Vec3 fallback) noexcept {
    const JsonValue* value = member(object, key);
    if (value == nullptr || value->kind != JsonKind::Array || value->array_values.size() < 3u) {
        return fallback;
    }
    math::Vec3 out = fallback;
    const JsonValue& x = value->array_values[0];
    const JsonValue& y = value->array_values[1];
    const JsonValue& z = value->array_values[2];
    if (x.kind == JsonKind::Number && y.kind == JsonKind::Number && z.kind == JsonKind::Number) {
        out = {static_cast<float>(x.number_value),
               static_cast<float>(y.number_value),
               static_cast<float>(z.number_value)};
    }
    return out;
}

void parse_float4(const JsonValue& object, std::string_view key, float out[4]) noexcept {
    const JsonValue* value = member(object, key);
    if (value == nullptr || value->kind != JsonKind::Array || value->array_values.size() < 4u) {
        return;
    }
    for (std::size_t i = 0; i < 4u; ++i) {
        if (value->array_values[i].kind != JsonKind::Number) {
            return;
        }
    }
    for (std::size_t i = 0; i < 4u; ++i) {
        out[i] = static_cast<float>(value->array_values[i].number_value);
    }
}

[[nodiscard]] float finite_or(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

[[nodiscard]] math::Vec3 sanitize_vec3(math::Vec3 value, math::Vec3 fallback) noexcept {
    value.x = finite_or(value.x, fallback.x);
    value.y = finite_or(value.y, fallback.y);
    value.z = finite_or(value.z, fallback.z);
    return value;
}

[[nodiscard]] math::Vec3 sanitize_color3(math::Vec3 value, math::Vec3 fallback) noexcept {
    value = sanitize_vec3(value, fallback);
    value.x = std::clamp(value.x, 0.0f, 1.0f);
    value.y = std::clamp(value.y, 0.0f, 1.0f);
    value.z = std::clamp(value.z, 0.0f, 1.0f);
    return value;
}

[[nodiscard]] math::Vec3 sanitize_direction(math::Vec3 value, math::Vec3 fallback) noexcept {
    value = sanitize_vec3(value, fallback);
    const float len = math::length(value);
    if (!std::isfinite(len) || len < 0.0001f) {
        return math::normalize(fallback);
    }
    return math::mul(value, 1.0f / len);
}

[[nodiscard]] bool parse_json_root(std::string_view text, JsonValue& out, ParseDiagnostics* diag) {
    ParseDiagnostics local_diag{};
    if (diag == nullptr) {
        diag = &local_diag;
    }
    *diag = {};

    Parser parser{text, 0, diag};
    if (!parser.parse_value(out)) {
        return false;
    }
    parser.skip_ws();
    if (parser.cursor != text.size()) {
        parser.set_error("trailing JSON data");
        return false;
    }
    if (out.kind != JsonKind::Object) {
        parser.set_error("loose scene root must be an object");
        return false;
    }

    diag->ok = true;
    return true;
}

[[nodiscard]] std::vector<std::string> parse_string_array(const JsonValue& object,
                                                          std::string_view key) {
    std::vector<std::string> out;
    const JsonValue* value = member(object, key);
    if (value == nullptr || value->kind != JsonKind::Array) {
        return out;
    }
    out.reserve(value->array_values.size());
    for (const JsonValue& item : value->array_values) {
        if (item.kind == JsonKind::String) {
            out.push_back(item.string_value);
        }
    }
    return out;
}

[[nodiscard]] bool array_contains(const std::vector<std::string>& values,
                                  std::string_view needle) noexcept {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

void ensure_component(std::vector<std::string>& components, std::string_view component) {
    if (!array_contains(components, component)) {
        components.emplace_back(component);
    }
}

[[nodiscard]] bool json_array_contains_string(const JsonValue& value,
                                              std::string_view needle) noexcept {
    if (value.kind != JsonKind::Array) {
        return false;
    }
    for (const JsonValue& item : value.array_values) {
        if (item.kind == JsonKind::String && item.string_value == needle) {
            return true;
        }
    }
    return false;
}

void json_array_ensure_string(JsonValue& value, std::string_view item) {
    if (value.kind != JsonKind::Array) {
        value = make_array();
    }
    if (!json_array_contains_string(value, item)) {
        value.array_values.push_back(make_string(item));
    }
}

void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    out.push_back('"');
}

void append_float(std::string& out, float value) {
    char buf[64]{};
    const int written = std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(value));
    if (written > 0) {
        out.append(buf, static_cast<std::size_t>(written));
    } else {
        out.push_back('0');
    }
}

void append_number(std::string& out, double value) {
    char buf[64]{};
    const int written = std::snprintf(buf, sizeof(buf), "%.17g", value);
    if (written > 0) {
        out.append(buf, static_cast<std::size_t>(written));
    } else {
        out.push_back('0');
    }
}

void append_vec3(std::string& out, math::Vec3 v) {
    out.push_back('[');
    append_float(out, v.x);
    out.append(", ");
    append_float(out, v.y);
    out.append(", ");
    append_float(out, v.z);
    out.push_back(']');
}

void append_float4(std::string& out, const float v[4]) {
    out.push_back('[');
    append_float(out, v[0]);
    out.append(", ");
    append_float(out, v[1]);
    out.append(", ");
    append_float(out, v[2]);
    out.append(", ");
    append_float(out, v[3]);
    out.push_back(']');
}

void append_indent(std::string& out, int indent) {
    for (int i = 0; i < indent; ++i) {
        out.push_back(' ');
    }
}

void append_json_value(std::string& out, const JsonValue& value, int indent);

void append_json_array(std::string& out, const JsonValue& value, int indent) {
    out.push_back('[');
    for (std::size_t i = 0; i < value.array_values.size(); ++i) {
        if (i != 0u) {
            out.append(", ");
        }
        append_json_value(out, value.array_values[i], indent);
    }
    out.push_back(']');
}

void append_json_object(std::string& out, const JsonValue& value, int indent) {
    out.push_back('{');
    if (!value.object_values.empty()) {
        out.push_back('\n');
    }
    for (std::size_t i = 0; i < value.object_values.size(); ++i) {
        const JsonMember& item = value.object_values[i];
        if (item.value_index >= value.array_values.size()) {
            continue;
        }
        append_indent(out, indent + 2);
        append_json_string(out, item.key);
        out.append(": ");
        append_json_value(out, value.array_values[item.value_index], indent + 2);
        if (i + 1u < value.object_values.size()) {
            out.push_back(',');
        }
        out.push_back('\n');
    }
    if (!value.object_values.empty()) {
        append_indent(out, indent);
    }
    out.push_back('}');
}

void append_json_value(std::string& out, const JsonValue& value, int indent) {
    switch (value.kind) {
        case JsonKind::Null:
            out.append("null");
            break;
        case JsonKind::Bool:
            out.append(value.bool_value ? "true" : "false");
            break;
        case JsonKind::Number:
            if (std::isfinite(value.number_value)) {
                append_number(out, value.number_value);
            } else {
                out.push_back('0');
            }
            break;
        case JsonKind::String:
            append_json_string(out, value.string_value);
            break;
        case JsonKind::Array:
            append_json_array(out, value, indent);
            break;
        case JsonKind::Object:
            append_json_object(out, value, indent);
            break;
    }
}

[[nodiscard]] std::string serialize_json_root(const JsonValue& root) {
    std::string out;
    out.reserve(1024u);
    append_json_value(out, root, 0);
    out.push_back('\n');
    return out;
}

[[nodiscard]] JsonValue vec3_json(math::Vec3 value) {
    JsonValue out = make_array();
    out.array_values.push_back(make_number(static_cast<double>(value.x)));
    out.array_values.push_back(make_number(static_cast<double>(value.y)));
    out.array_values.push_back(make_number(static_cast<double>(value.z)));
    return out;
}

[[nodiscard]] JsonValue float4_json(const float value[4]) {
    JsonValue out = make_array();
    for (std::size_t i = 0; i < 4u; ++i) {
        out.array_values.push_back(make_number(static_cast<double>(value[i])));
    }
    return out;
}

[[nodiscard]] JsonValue viewport_camera_json(ViewportCameraState camera) {
    sanitize_viewport_camera(camera);
    JsonValue out = make_object();
    set_object_member(out, "position", vec3_json(camera.position));
    set_object_member(out, "rotation_euler_deg", vec3_json(camera.rotation_euler_deg));
    set_object_member(out, "fov_y_deg", make_number(static_cast<double>(camera.fov_y_deg)));
    return out;
}

[[nodiscard]] JsonValue material_json(MaterialSettings material) {
    sanitize_material(material);
    JsonValue out = make_object();
    set_object_member(out, "albedo", vec3_json(material.albedo));
    set_object_member(out, "roughness", make_number(static_cast<double>(material.roughness)));
    set_object_member(out, "metallic", make_number(static_cast<double>(material.metallic)));
    set_object_member(out, "emissive", vec3_json(material.emissive));
    set_object_member(out,
                      "emissive_intensity",
                      make_number(static_cast<double>(material.emissive_intensity)));
    return out;
}

[[nodiscard]] JsonValue player_controller_json(PlayerControllerSettings player_controller) {
    sanitize_player_controller(player_controller);
    JsonValue out = make_object();
    set_object_member(out,
                      "walk_speed",
                      make_number(static_cast<double>(player_controller.walk_speed)));
    set_object_member(out,
                      "run_speed",
                      make_number(static_cast<double>(player_controller.run_speed)));
    set_object_member(out,
                      "jump_velocity",
                      make_number(static_cast<double>(player_controller.jump_velocity)));
    set_object_member(out,
                      "mouse_sensitivity",
                      make_number(static_cast<double>(player_controller.mouse_sensitivity)));
    set_object_member(out,
                      "capsule_radius",
                      make_number(static_cast<double>(player_controller.capsule_radius)));
    set_object_member(out,
                      "capsule_height",
                      make_number(static_cast<double>(player_controller.capsule_height)));
    return out;
}

[[nodiscard]] JsonValue clouds_json(CloudSettings clouds) {
    sanitize_clouds(clouds);
    JsonValue out = make_object();
    set_object_member(out, "coverage", make_number(static_cast<double>(clouds.coverage)));
    set_object_member(out, "density", make_number(static_cast<double>(clouds.density)));
    set_object_member(out, "speed", make_number(static_cast<double>(clouds.speed)));
    set_object_member(out, "height_m", make_number(static_cast<double>(clouds.height_m)));
    return out;
}

[[nodiscard]] bool json_entity_exists(const JsonValue& root, std::string_view entity_name) noexcept {
    if (entity_name.empty()) {
        return false;
    }
    const JsonValue* entities = member(root, "entities");
    if (entities == nullptr || entities->kind != JsonKind::Array) {
        return false;
    }
    for (const JsonValue& entity : entities->array_values) {
        if (entity.kind != JsonKind::Object) {
            continue;
        }
        const JsonValue* name = member(entity, "name");
        if (name != nullptr && name->kind == JsonKind::String && name->string_value == entity_name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool json_entity_has_camera(const JsonValue& entity) noexcept {
    if (entity.kind != JsonKind::Object) {
        return false;
    }
    const JsonValue* components = member(entity, "components");
    if (components == nullptr || components->kind != JsonKind::Array) {
        return false;
    }
    for (const JsonValue& component : components->array_values) {
        if (component.kind == JsonKind::String && component.string_value == "Camera") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool json_camera_exists(const JsonValue& root, std::string_view entity_name) noexcept {
    if (entity_name.empty()) {
        return false;
    }
    const JsonValue* entities = member(root, "entities");
    if (entities == nullptr || entities->kind != JsonKind::Array) {
        return false;
    }
    for (const JsonValue& entity : entities->array_values) {
        if (!json_entity_has_camera(entity)) {
            continue;
        }
        const JsonValue* name = member(entity, "name");
        if (name != nullptr && name->kind == JsonKind::String && name->string_value == entity_name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string first_json_camera_name(const JsonValue& root) {
    const JsonValue* entities = member(root, "entities");
    if (entities == nullptr || entities->kind != JsonKind::Array) {
        return {};
    }
    for (const JsonValue& entity : entities->array_values) {
        if (!json_entity_has_camera(entity)) {
            continue;
        }
        const JsonValue* name = member(entity, "name");
        if (name != nullptr && name->kind == JsonKind::String && !name->string_value.empty()) {
            return name->string_value;
        }
    }
    return {};
}

[[nodiscard]] ViewportCameraState viewport_camera_or(const JsonValue& object,
                                                     ViewportCameraState fallback) noexcept {
    ViewportCameraState out = fallback;
    out.position = vec3_or(object, "position", out.position);
    out.rotation_euler_deg = vec3_or(object, "rotation_euler_deg", out.rotation_euler_deg);
    out.fov_y_deg = float_or(object, "fov_y_deg", out.fov_y_deg);
    sanitize_viewport_camera(out);
    return out;
}

[[nodiscard]] MaterialSettings material_or(const JsonValue& object,
                                           MaterialSettings fallback) noexcept {
    MaterialSettings out = fallback;
    out.albedo = vec3_or(object, "albedo", out.albedo);
    out.roughness = float_or(object, "roughness", out.roughness);
    out.metallic = float_or(object, "metallic", out.metallic);
    out.emissive = vec3_or(object, "emissive", out.emissive);
    out.emissive_intensity = float_or(object, "emissive_intensity", out.emissive_intensity);
    sanitize_material(out);
    return out;
}

[[nodiscard]] PlayerControllerSettings player_controller_or(
    const JsonValue& object,
    PlayerControllerSettings fallback) noexcept {
    PlayerControllerSettings out = fallback;
    out.walk_speed = float_or(object, "walk_speed", out.walk_speed);
    out.run_speed = float_or(object, "run_speed", out.run_speed);
    out.jump_velocity = float_or(object, "jump_velocity", out.jump_velocity);
    out.mouse_sensitivity = float_or(object, "mouse_sensitivity", out.mouse_sensitivity);
    out.capsule_radius = float_or(object, "capsule_radius", out.capsule_radius);
    out.capsule_height = float_or(object, "capsule_height", out.capsule_height);
    sanitize_player_controller(out);
    return out;
}

[[nodiscard]] CloudSettings clouds_or(const JsonValue& object, CloudSettings fallback) noexcept {
    CloudSettings out = fallback;
    out.coverage = float_or(object, "coverage", out.coverage);
    out.density = float_or(object, "density", out.density);
    out.speed = float_or(object, "speed", out.speed);
    out.height_m = float_or(object, "height_m", out.height_m);
    sanitize_clouds(out);
    return out;
}

void sanitize_json_authoring_state(JsonValue& root) {
    if (root.kind != JsonKind::Object) {
        return;
    }

    const int version =
        sanitize_scene_document_version(int_or(root, "version", k_current_scene_document_version));
    set_object_member(root, "version", make_number(static_cast<double>(version)));

    JsonValue& editor = ensure_object_member(root, "editorSettings");
    const bool grid_visible = bool_or(editor, "grid_visible", true);
    const SnapMode snap_mode = snap_mode_from_token(string_or(editor, "snap_mode", "off"));
    const float snap_step = sanitize_snap_step(float_or(editor, "snap_step", 1.0f));
    const TransformMode transform_mode =
        transform_mode_from_token(string_or(editor, "transform_mode", "translate"));
    const RenderDebugMode render_debug =
        render_debug_mode_from_token(string_or(editor, "render_debug", "off"));
    set_object_member(editor, "grid_visible", make_bool(grid_visible));
    set_object_member(editor, "snap_mode", make_string(snap_mode_token(snap_mode)));
    set_object_member(editor, "snap_step", make_number(static_cast<double>(snap_step)));
    set_object_member(editor, "transform_mode", make_string(transform_mode_token(transform_mode)));
    set_object_member(editor, "render_debug", make_string(render_debug_mode_token(render_debug)));
    if (const JsonValue* camera = member(editor, "viewport_camera");
        camera != nullptr && camera->kind == JsonKind::Object) {
        set_object_member(editor,
                          "viewport_camera",
                          viewport_camera_json(viewport_camera_or(*camera, {})));
    }

    const JsonValue* selected = member(root, "selected_entity");
    if (selected == nullptr || selected->kind != JsonKind::String ||
        !json_entity_exists(root, selected->string_value)) {
        set_object_member(root, "selected_entity", JsonValue{});
    }

    const JsonValue* active = member(root, "active_camera");
    if (active != nullptr && active->kind == JsonKind::Null) {
        set_object_member(root, "active_camera", JsonValue{});
    } else if (active == nullptr || active->kind != JsonKind::String ||
               !json_camera_exists(root, active->string_value)) {
        const std::string first_camera = first_json_camera_name(root);
        if (first_camera.empty()) {
            set_object_member(root, "active_camera", JsonValue{});
        } else {
            set_object_member(root, "active_camera", make_string(first_camera));
        }
    }

    JsonValue& environment = ensure_object_member(root, "environmentSettings");
    float clear_color[4] = {0.018f, 0.027f, 0.050f, 1.0f};
    parse_float4(environment, "clear_color", clear_color);
    sanitize_clear_color(clear_color);
    set_object_member(environment, "clear_color", float4_json(clear_color));
    if (const JsonValue* sky = member(environment, "sky_mode");
        sky == nullptr || sky->kind != JsonKind::String) {
        set_object_member(environment, "sky_mode", make_string("sdf-boot-field"));
    }
    EnvironmentSettings env_defaults{};
    env_defaults.time_of_day_hours =
        std::fmod(float_or(environment, "time_of_day_hours", env_defaults.time_of_day_hours), 24.0f);
    if (env_defaults.time_of_day_hours < 0.0f) {
        env_defaults.time_of_day_hours += 24.0f;
    }
    env_defaults.sun_direction =
        vec3_or(environment, "sun_direction", env_defaults.sun_direction);
    env_defaults.sun_color = vec3_or(environment, "sun_color", env_defaults.sun_color);
    env_defaults.sun_intensity =
        float_or(environment, "sun_intensity", env_defaults.sun_intensity);
    env_defaults.ambient_color =
        vec3_or(environment, "ambient_color", env_defaults.ambient_color);
    env_defaults.ambient_intensity =
        float_or(environment, "ambient_intensity", env_defaults.ambient_intensity);
    env_defaults.exposure = float_or(environment, "exposure", env_defaults.exposure);
    if (const JsonValue* clouds = member(environment, "clouds");
        clouds != nullptr && clouds->kind == JsonKind::Object) {
        env_defaults.clouds = clouds_or(*clouds, env_defaults.clouds);
    }
    sanitize_environment(env_defaults);
    set_object_member(environment,
                      "time_of_day_hours",
                      make_number(static_cast<double>(env_defaults.time_of_day_hours)));
    set_object_member(environment, "sun_direction", vec3_json(env_defaults.sun_direction));
    set_object_member(environment, "sun_color", vec3_json(env_defaults.sun_color));
    set_object_member(environment,
                      "sun_intensity",
                      make_number(static_cast<double>(env_defaults.sun_intensity)));
    set_object_member(environment, "ambient_color", vec3_json(env_defaults.ambient_color));
    set_object_member(environment,
                      "ambient_intensity",
                      make_number(static_cast<double>(env_defaults.ambient_intensity)));
    set_object_member(environment,
                      "exposure",
                      make_number(static_cast<double>(env_defaults.exposure)));
    set_object_member(environment, "clouds", clouds_json(env_defaults.clouds));

    if (JsonValue* entities = mutable_member(root, "entities");
        entities != nullptr && entities->kind == JsonKind::Array) {
        for (JsonValue& entity : entities->array_values) {
            if (entity.kind != JsonKind::Object) {
                continue;
            }
            if (JsonValue* components = mutable_member(entity, "components");
                components != nullptr && json_array_contains_string(*components, "PlayerStart")) {
                json_array_ensure_string(*components, "Transform");
            }
            if (JsonValue* components = mutable_member(entity, "components");
                components != nullptr && json_array_contains_string(*components, "PlayerController")) {
                json_array_ensure_string(*components, "Transform");
                PlayerControllerSettings player_controller{};
                if (const JsonValue* player_controller_object = member(entity, "player_controller");
                    player_controller_object != nullptr &&
                    player_controller_object->kind == JsonKind::Object) {
                    player_controller =
                        player_controller_or(*player_controller_object, player_controller);
                }
                set_object_member(entity,
                                  "player_controller",
                                  player_controller_json(player_controller));
            }
            MaterialSettings material{};
            if (const JsonValue* material_object = member(entity, "material");
                material_object != nullptr && material_object->kind == JsonKind::Object) {
                material = material_or(*material_object, material);
            }
            set_object_member(entity, "material", material_json(material));
        }
    }
}

}  // namespace

bool SceneEntity::has_camera() const noexcept {
    return array_contains(components, "Camera");
}

bool SceneEntity::has_primitive() const noexcept {
    return primitive != PrimitiveKind::None;
}

bool SceneEntity::has_player_start() const noexcept {
    return array_contains(components, "PlayerStart");
}

bool SceneEntity::has_player_controller() const noexcept {
    return array_contains(components, "PlayerController");
}

bool SceneEntity::selected(std::string_view selected_entity) const noexcept {
    return !selected_entity.empty() && name == selected_entity;
}

PrimitiveKind primitive_kind_from_token(std::string_view token) noexcept {
    if (token == "cube") {
        return PrimitiveKind::Cube;
    }
    if (token == "sphere") {
        return PrimitiveKind::Sphere;
    }
    if (token == "plane") {
        return PrimitiveKind::Plane;
    }
    if (token == "capsule") {
        return PrimitiveKind::Capsule;
    }
    if (token.empty()) {
        return PrimitiveKind::None;
    }
    return PrimitiveKind::Unknown;
}

std::string_view primitive_kind_token(PrimitiveKind kind) noexcept {
    switch (kind) {
        case PrimitiveKind::Cube:
            return "cube";
        case PrimitiveKind::Sphere:
            return "sphere";
        case PrimitiveKind::Plane:
            return "plane";
        case PrimitiveKind::Capsule:
            return "capsule";
        case PrimitiveKind::None:
            return "";
        case PrimitiveKind::Unknown:
            return "";
    }
    return "";
}

SnapMode snap_mode_from_token(std::string_view token) noexcept {
    if (token == "grid") {
        return SnapMode::Grid;
    }
    return SnapMode::Off;
}

std::string_view snap_mode_token(SnapMode mode) noexcept {
    switch (mode) {
        case SnapMode::Grid:
            return "grid";
        case SnapMode::Off:
            return "off";
    }
    return "off";
}

TransformMode transform_mode_from_token(std::string_view token) noexcept {
    if (token == "rotate") {
        return TransformMode::Rotate;
    }
    if (token == "scale") {
        return TransformMode::Scale;
    }
    return TransformMode::Translate;
}

std::string_view transform_mode_token(TransformMode mode) noexcept {
    switch (mode) {
        case TransformMode::Rotate:
            return "rotate";
        case TransformMode::Scale:
            return "scale";
        case TransformMode::Translate:
            return "translate";
    }
    return "translate";
}

RenderDebugMode render_debug_mode_from_token(std::string_view token) noexcept {
    if (token == "depth") {
        return RenderDebugMode::Depth;
    }
    return RenderDebugMode::Off;
}

std::string_view render_debug_mode_token(RenderDebugMode mode) noexcept {
    switch (mode) {
        case RenderDebugMode::Depth:
            return "depth";
        case RenderDebugMode::Off:
            return "off";
    }
    return "off";
}

int sanitize_scene_document_version(int version) noexcept {
    if (version <= 0) {
        return k_current_scene_document_version;
    }
    return std::min(version, k_current_scene_document_version);
}

float sanitize_snap_step(float snap_step) noexcept {
    if (!std::isfinite(snap_step)) {
        return 1.0f;
    }
    return std::clamp(snap_step, 0.001f, 100.0f);
}

float sanitize_fov_y_deg(float fov_y_deg) noexcept {
    if (!std::isfinite(fov_y_deg)) {
        return 70.0f;
    }
    return std::clamp(fov_y_deg, 1.0f, 179.0f);
}

void sanitize_clear_color(float clear_color[4]) noexcept {
    for (std::size_t i = 0; i < 4u; ++i) {
        if (!std::isfinite(clear_color[i])) {
            clear_color[i] = i == 3u ? 1.0f : 0.0f;
        }
        clear_color[i] = std::clamp(clear_color[i], 0.0f, 1.0f);
    }
}

void sanitize_viewport_camera(ViewportCameraState& camera) noexcept {
    camera.position = sanitize_vec3(camera.position, {0.0f, 1.4f, 6.0f});
    camera.rotation_euler_deg = sanitize_vec3(camera.rotation_euler_deg, {-10.0f, 180.0f, 0.0f});
    camera.fov_y_deg = sanitize_fov_y_deg(camera.fov_y_deg);
}

void sanitize_transform(Transform& transform) noexcept {
    transform.position = sanitize_vec3(transform.position, {0.0f, 0.0f, 0.0f});
    transform.rotation_euler_deg = sanitize_vec3(transform.rotation_euler_deg, {0.0f, 0.0f, 0.0f});
    transform.scale = sanitize_vec3(transform.scale, {1.0f, 1.0f, 1.0f});
}

void sanitize_material(MaterialSettings& material) noexcept {
    material.albedo = sanitize_color3(material.albedo, {0.78f, 0.78f, 0.78f});
    material.roughness = finite_or(material.roughness, 0.55f);
    material.roughness = std::clamp(material.roughness, 0.02f, 1.0f);
    material.metallic = finite_or(material.metallic, 0.0f);
    material.metallic = std::clamp(material.metallic, 0.0f, 1.0f);
    material.emissive = sanitize_color3(material.emissive, {0.0f, 0.0f, 0.0f});
    material.emissive_intensity = finite_or(material.emissive_intensity, 0.0f);
    material.emissive_intensity = std::clamp(material.emissive_intensity, 0.0f, 128.0f);
}

void sanitize_player_controller(PlayerControllerSettings& player_controller) noexcept {
    player_controller.walk_speed = finite_or(player_controller.walk_speed, 4.6f);
    player_controller.run_speed = finite_or(player_controller.run_speed, 7.0f);
    player_controller.jump_velocity = finite_or(player_controller.jump_velocity, 5.2f);
    player_controller.mouse_sensitivity = finite_or(player_controller.mouse_sensitivity, 0.10f);
    player_controller.capsule_radius = finite_or(player_controller.capsule_radius, 0.32f);
    player_controller.capsule_height = finite_or(player_controller.capsule_height, 1.75f);

    player_controller.walk_speed = std::clamp(player_controller.walk_speed, 0.0f, 64.0f);
    player_controller.run_speed =
        std::clamp(player_controller.run_speed, player_controller.walk_speed, 96.0f);
    player_controller.jump_velocity = std::clamp(player_controller.jump_velocity, 0.0f, 64.0f);
    player_controller.mouse_sensitivity =
        std::clamp(player_controller.mouse_sensitivity, 0.001f, 4.0f);
    player_controller.capsule_radius = std::clamp(player_controller.capsule_radius, 0.05f, 4.0f);
    player_controller.capsule_height =
        std::clamp(player_controller.capsule_height,
                   player_controller.capsule_radius * 2.0f,
                   8.0f);
}

void sanitize_clouds(CloudSettings& clouds) noexcept {
    clouds.coverage = finite_or(clouds.coverage, 0.42f);
    clouds.coverage = std::clamp(clouds.coverage, 0.0f, 1.0f);
    clouds.density = finite_or(clouds.density, 0.55f);
    clouds.density = std::clamp(clouds.density, 0.0f, 4.0f);
    clouds.speed = finite_or(clouds.speed, 0.035f);
    clouds.speed = std::clamp(clouds.speed, -1.0f, 1.0f);
    clouds.height_m = finite_or(clouds.height_m, 1800.0f);
    clouds.height_m = std::clamp(clouds.height_m, 50.0f, 20000.0f);
}

void sanitize_environment(EnvironmentSettings& environment) noexcept {
    sanitize_clear_color(environment.clear_color);
    if (environment.sky_mode.empty()) {
        environment.sky_mode = "sdf-boot-field";
    }
    environment.time_of_day_hours = finite_or(environment.time_of_day_hours, 14.0f);
    environment.time_of_day_hours = std::fmod(environment.time_of_day_hours, 24.0f);
    if (environment.time_of_day_hours < 0.0f) {
        environment.time_of_day_hours += 24.0f;
    }
    environment.sun_direction =
        sanitize_direction(environment.sun_direction, {0.25f, -0.78f, 0.57f});
    environment.sun_color = sanitize_color3(environment.sun_color, {1.0f, 0.93f, 0.82f});
    environment.sun_intensity = finite_or(environment.sun_intensity, 4.0f);
    environment.sun_intensity = std::clamp(environment.sun_intensity, 0.0f, 256.0f);
    environment.ambient_color =
        sanitize_color3(environment.ambient_color, {0.20f, 0.25f, 0.35f});
    environment.ambient_intensity = finite_or(environment.ambient_intensity, 0.35f);
    environment.ambient_intensity = std::clamp(environment.ambient_intensity, 0.0f, 16.0f);
    environment.exposure = finite_or(environment.exposure, 1.0f);
    environment.exposure = std::clamp(environment.exposure, 0.01f, 64.0f);
    sanitize_clouds(environment.clouds);
}

LooseSceneDocument make_default_scene_document() {
    LooseSceneDocument scene{};
    scene.version = k_current_scene_document_version;
    scene.active_camera = "Camera";
    scene.active_camera_is_null = false;
    scene.selected_entity_is_null = true;
    scene.editor_settings.has_viewport_camera = true;
    sanitize_viewport_camera(scene.editor_settings.viewport_camera);
    scene.environment_settings.sky_mode = "sdf-boot-field";
    sanitize_environment(scene.environment_settings);

    SceneEntity camera{};
    camera.name = "Camera";
    camera.components = {"Transform", "Camera"};
    camera.transform.position = {0.0f, 1.4f, 4.2f};
    camera.transform.rotation_euler_deg = {-8.0f, 180.0f, 0.0f};
    camera.fov_y_deg = 70.0f;
    scene.entities.push_back(std::move(camera));
    return scene;
}

bool parse_loose_scene_document(std::string_view scene_json,
                                LooseSceneDocument& out,
                                ParseDiagnostics* diag) {
    ParseDiagnostics local_diag{};
    if (diag == nullptr) {
        diag = &local_diag;
    }
    *diag = {};

    JsonValue root{};
    if (!parse_json_root(scene_json, root, diag)) {
        return false;
    }

    LooseSceneDocument scene = make_default_scene_document();
    scene.version =
        sanitize_scene_document_version(int_or(root, "version", scene.version));
    scene.format = string_or(root, "format", scene.format);
    scene.name = string_or(root, "name", scene.name);
    scene.path = string_or(root, "path", scene.path);

    if (const JsonValue* active = member(root, "active_camera")) {
        scene.active_camera.clear();
        scene.active_camera_is_null = active->kind == JsonKind::Null;
        if (active->kind == JsonKind::String) {
            scene.active_camera = active->string_value;
            scene.active_camera_is_null = false;
        }
    }
    if (const JsonValue* selected = member(root, "selected_entity")) {
        scene.selected_entity.clear();
        scene.selected_entity_is_null = selected->kind == JsonKind::Null;
        if (selected->kind == JsonKind::String) {
            scene.selected_entity = selected->string_value;
            scene.selected_entity_is_null = selected->string_value.empty();
        }
    }

    if (const JsonValue* editor = member(root, "editorSettings")) {
        scene.editor_settings.grid_visible =
            bool_or(*editor, "grid_visible", scene.editor_settings.grid_visible);
        scene.editor_settings.snap_mode =
            snap_mode_from_token(string_or(*editor, "snap_mode", "off"));
        scene.editor_settings.snap_step =
            sanitize_snap_step(float_or(*editor, "snap_step", scene.editor_settings.snap_step));
        scene.editor_settings.transform_mode =
            transform_mode_from_token(string_or(*editor, "transform_mode", "translate"));
        scene.editor_settings.render_debug =
            render_debug_mode_from_token(string_or(*editor, "render_debug", "off"));
        if (const JsonValue* viewport_camera = member(*editor, "viewport_camera");
            viewport_camera != nullptr && viewport_camera->kind == JsonKind::Object) {
            scene.editor_settings.viewport_camera =
                viewport_camera_or(*viewport_camera, scene.editor_settings.viewport_camera);
            scene.editor_settings.has_viewport_camera = true;
        }
    }
    if (const JsonValue* environment = member(root, "environmentSettings")) {
        parse_float4(*environment, "clear_color", scene.environment_settings.clear_color);
        scene.environment_settings.sky_mode =
            string_or(*environment, "sky_mode", scene.environment_settings.sky_mode);
        scene.environment_settings.time_of_day_hours =
            float_or(*environment,
                     "time_of_day_hours",
                     scene.environment_settings.time_of_day_hours);
        scene.environment_settings.sun_direction =
            vec3_or(*environment, "sun_direction", scene.environment_settings.sun_direction);
        scene.environment_settings.sun_color =
            vec3_or(*environment, "sun_color", scene.environment_settings.sun_color);
        scene.environment_settings.sun_intensity =
            float_or(*environment, "sun_intensity", scene.environment_settings.sun_intensity);
        scene.environment_settings.ambient_color =
            vec3_or(*environment, "ambient_color", scene.environment_settings.ambient_color);
        scene.environment_settings.ambient_intensity =
            float_or(*environment,
                     "ambient_intensity",
                     scene.environment_settings.ambient_intensity);
        scene.environment_settings.exposure =
            float_or(*environment, "exposure", scene.environment_settings.exposure);
        if (const JsonValue* clouds = member(*environment, "clouds");
            clouds != nullptr && clouds->kind == JsonKind::Object) {
            scene.environment_settings.clouds =
                clouds_or(*clouds, scene.environment_settings.clouds);
        }
        sanitize_environment(scene.environment_settings);
    }

    scene.entities.clear();
    if (const JsonValue* entities = member(root, "entities");
        entities != nullptr && entities->kind == JsonKind::Array) {
        scene.entities.reserve(entities->array_values.size());
        for (const JsonValue& entity_json : entities->array_values) {
            if (entity_json.kind != JsonKind::Object) {
                continue;
            }
            SceneEntity entity{};
            entity.name = string_or(entity_json, "name", {});
            entity.components = parse_string_array(entity_json, "components");
            const std::string primitive_token = string_or(entity_json, "primitive", {});
            entity.primitive = primitive_kind_from_token(primitive_token);
            entity.primitive_token = entity.primitive == PrimitiveKind::Unknown
                                         ? primitive_token
                                         : std::string{primitive_kind_token(entity.primitive)};
            entity.transform.position = vec3_or(entity_json, "position", entity.transform.position);
            entity.transform.rotation_euler_deg =
                vec3_or(entity_json, "rotation_euler_deg", entity.transform.rotation_euler_deg);
            entity.transform.scale = vec3_or(entity_json, "scale", entity.transform.scale);
            sanitize_transform(entity.transform);
            if (const JsonValue* material = member(entity_json, "material");
                material != nullptr && material->kind == JsonKind::Object) {
                entity.material = material_or(*material, entity.material);
            }
            if (const JsonValue* player_controller = member(entity_json, "player_controller");
                player_controller != nullptr && player_controller->kind == JsonKind::Object) {
                entity.player_controller =
                    player_controller_or(*player_controller, entity.player_controller);
            }
            entity.fov_y_deg = sanitize_fov_y_deg(float_or(entity_json, "fov_y_deg", entity.fov_y_deg));
            entity.visible = bool_or(entity_json, "visible", true);
            scene.entities.push_back(std::move(entity));
        }
    }

    normalize_scene_document(scene);
    out = std::move(scene);
    diag->ok = true;
    return true;
}

std::string serialize_loose_scene_document(const LooseSceneDocument& scene) {
    LooseSceneDocument normalized = scene;
    normalize_scene_document(normalized);
    std::string out;
    out.reserve(512u + normalized.entities.size() * 256u);
    out.append("{\n");
    out.append("  \"version\": ");
    append_float(out, static_cast<float>(normalized.version));
    out.append(",\n");
    out.append("  \"format\": ");
    append_json_string(out, normalized.format);
    out.append(",\n  \"name\": ");
    append_json_string(out, normalized.name);
    out.append(",\n  \"path\": ");
    append_json_string(out, normalized.path);
    out.append(",\n  \"active_camera\": ");
    if (normalized.active_camera_is_null || normalized.active_camera.empty()) {
        out.append("null");
    } else {
        append_json_string(out, normalized.active_camera);
    }
    out.append(",\n  \"selected_entity\": ");
    if (normalized.selected_entity_is_null || normalized.selected_entity.empty()) {
        out.append("null");
    } else {
        append_json_string(out, normalized.selected_entity);
    }
    out.append(",\n  \"editorSettings\": {\n");
    out.append("    \"grid_visible\": ");
    out.append(normalized.editor_settings.grid_visible ? "true" : "false");
    out.append(",\n    \"snap_mode\": ");
    append_json_string(out, snap_mode_token(normalized.editor_settings.snap_mode));
    out.append(",\n    \"snap_step\": ");
    append_float(out, normalized.editor_settings.snap_step);
    out.append(",\n    \"transform_mode\": ");
    append_json_string(out, transform_mode_token(normalized.editor_settings.transform_mode));
    out.append(",\n    \"render_debug\": ");
    append_json_string(out, render_debug_mode_token(normalized.editor_settings.render_debug));
    if (normalized.editor_settings.has_viewport_camera) {
        ViewportCameraState camera = normalized.editor_settings.viewport_camera;
        out.append(",\n    \"viewport_camera\": {\n");
        out.append("      \"position\": ");
        append_vec3(out, camera.position);
        out.append(",\n      \"rotation_euler_deg\": ");
        append_vec3(out, camera.rotation_euler_deg);
        out.append(",\n      \"fov_y_deg\": ");
        append_float(out, camera.fov_y_deg);
        out.append("\n    }");
    }
    out.append("\n  },\n  \"environmentSettings\": {\n");
    out.append("    \"clear_color\": ");
    const float clear_color[4] = {normalized.environment_settings.clear_color[0],
                                  normalized.environment_settings.clear_color[1],
                                  normalized.environment_settings.clear_color[2],
                                  normalized.environment_settings.clear_color[3]};
    append_float4(out, clear_color);
    if (!normalized.environment_settings.sky_mode.empty()) {
        out.append(",\n    \"sky_mode\": ");
        append_json_string(out, normalized.environment_settings.sky_mode);
    }
    out.append(",\n    \"time_of_day_hours\": ");
    append_float(out, normalized.environment_settings.time_of_day_hours);
    out.append(",\n    \"sun_direction\": ");
    append_vec3(out, normalized.environment_settings.sun_direction);
    out.append(",\n    \"sun_color\": ");
    append_vec3(out, normalized.environment_settings.sun_color);
    out.append(",\n    \"sun_intensity\": ");
    append_float(out, normalized.environment_settings.sun_intensity);
    out.append(",\n    \"ambient_color\": ");
    append_vec3(out, normalized.environment_settings.ambient_color);
    out.append(",\n    \"ambient_intensity\": ");
    append_float(out, normalized.environment_settings.ambient_intensity);
    out.append(",\n    \"exposure\": ");
    append_float(out, normalized.environment_settings.exposure);
    out.append(",\n    \"clouds\": {\n");
    out.append("      \"coverage\": ");
    append_float(out, normalized.environment_settings.clouds.coverage);
    out.append(",\n      \"density\": ");
    append_float(out, normalized.environment_settings.clouds.density);
    out.append(",\n      \"speed\": ");
    append_float(out, normalized.environment_settings.clouds.speed);
    out.append(",\n      \"height_m\": ");
    append_float(out, normalized.environment_settings.clouds.height_m);
    out.append("\n    }");
    out.append("\n  },\n  \"entities\": [");
    for (std::size_t i = 0; i < normalized.entities.size(); ++i) {
        const SceneEntity& entity = normalized.entities[i];
        out.append(i == 0 ? "\n" : ",\n");
        out.append("    {\n");
        out.append("      \"name\": ");
        append_json_string(out, entity.name);
        out.append(",\n      \"components\": [");
        for (std::size_t c = 0; c < entity.components.size(); ++c) {
            if (c != 0u) {
                out.append(", ");
            }
            append_json_string(out, entity.components[c]);
        }
        out.append("]");
        if (entity.has_primitive()) {
            out.append(",\n      \"primitive\": ");
            const std::string_view token = entity.primitive == PrimitiveKind::Unknown
                                               ? std::string_view{entity.primitive_token}
                                               : primitive_kind_token(entity.primitive);
            append_json_string(out, token);
        }
        out.append(",\n      \"position\": ");
        append_vec3(out, entity.transform.position);
        out.append(",\n      \"rotation_euler_deg\": ");
        append_vec3(out, entity.transform.rotation_euler_deg);
        out.append(",\n      \"scale\": ");
        append_vec3(out, entity.transform.scale);
        if (entity.has_primitive()) {
            out.append(",\n      \"material\": {\n");
            out.append("        \"albedo\": ");
            append_vec3(out, entity.material.albedo);
            out.append(",\n        \"roughness\": ");
            append_float(out, entity.material.roughness);
            out.append(",\n        \"metallic\": ");
            append_float(out, entity.material.metallic);
            out.append(",\n        \"emissive\": ");
            append_vec3(out, entity.material.emissive);
            out.append(",\n        \"emissive_intensity\": ");
            append_float(out, entity.material.emissive_intensity);
            out.append("\n      }");
        }
        if (entity.has_camera()) {
            out.append(",\n      \"fov_y_deg\": ");
            append_float(out, entity.fov_y_deg);
        }
        if (entity.has_player_controller()) {
            out.append(",\n      \"player_controller\": {\n");
            out.append("        \"walk_speed\": ");
            append_float(out, entity.player_controller.walk_speed);
            out.append(",\n        \"run_speed\": ");
            append_float(out, entity.player_controller.run_speed);
            out.append(",\n        \"jump_velocity\": ");
            append_float(out, entity.player_controller.jump_velocity);
            out.append(",\n        \"mouse_sensitivity\": ");
            append_float(out, entity.player_controller.mouse_sensitivity);
            out.append(",\n        \"capsule_radius\": ");
            append_float(out, entity.player_controller.capsule_radius);
            out.append(",\n        \"capsule_height\": ");
            append_float(out, entity.player_controller.capsule_height);
            out.append("\n      }");
        }
        if (!entity.visible) {
            out.append(",\n      \"visible\": false");
        }
        out.append("\n    }");
    }
    if (!normalized.entities.empty()) {
        out.append("\n  ]\n");
    } else {
        out.append("]\n");
    }
    out.append("}\n");
    return out;
}

RuntimeSceneView make_runtime_scene_view(const LooseSceneDocument& scene) {
    LooseSceneDocument normalized = scene;
    normalize_scene_document(normalized);
    RuntimeSceneView out{};
    std::copy(std::begin(normalized.environment_settings.clear_color),
              std::end(normalized.environment_settings.clear_color),
              std::begin(out.clear_color));
    sanitize_clear_color(out.clear_color);
    out.environment = normalized.environment_settings;
    sanitize_environment(out.environment);
    out.grid_visible = normalized.editor_settings.grid_visible;
    out.snap_mode = normalized.editor_settings.snap_mode;
    out.snap_step = normalized.editor_settings.snap_step;
    out.transform_mode = normalized.editor_settings.transform_mode;
    out.render_debug = normalized.editor_settings.render_debug;
    out.primitives.reserve(normalized.entities.size());
    out.player_starts.reserve(normalized.entities.size());
    out.player_controllers.reserve(normalized.entities.size());

    for (const SceneEntity& entity : normalized.entities) {
        if (entity.has_camera() && !normalized.active_camera_is_null &&
            entity.name == normalized.active_camera) {
            out.active_camera.name = entity.name;
            out.active_camera.transform = entity.transform;
            out.active_camera.fov_y_deg = entity.fov_y_deg;
            out.active_camera.active = true;
            out.has_active_camera = true;
        }
        if (entity.has_primitive()) {
            PrimitiveView primitive{};
            primitive.name = entity.name;
            primitive.kind = entity.primitive;
            primitive.primitive_token = entity.primitive_token;
            primitive.transform = entity.transform;
            primitive.material = entity.material;
            primitive.visible = entity.visible;
            primitive.selected = entity.selected(normalized.selected_entity);
            out.primitives.push_back(std::move(primitive));
        }
        if (entity.has_player_start()) {
            PlayerStartView player_start{};
            player_start.name = entity.name;
            player_start.transform = entity.transform;
            player_start.selected = entity.selected(normalized.selected_entity);
            out.player_starts.push_back(std::move(player_start));
        }
        if (entity.has_player_controller()) {
            PlayerControllerView player_controller{};
            player_controller.name = entity.name;
            player_controller.transform = entity.transform;
            player_controller.settings = entity.player_controller;
            player_controller.selected = entity.selected(normalized.selected_entity);
            out.player_controllers.push_back(std::move(player_controller));
        }
    }

    if (!out.has_active_camera && !normalized.active_camera_is_null && normalized.active_camera.empty()) {
        for (const SceneEntity& entity : normalized.entities) {
            if (!entity.has_camera()) {
                continue;
            }
            out.active_camera.name = entity.name;
            out.active_camera.transform = entity.transform;
            out.active_camera.fov_y_deg = entity.fov_y_deg;
            out.active_camera.active = true;
            out.has_active_camera = true;
            break;
        }
    }
    return out;
}

bool parse_runtime_scene_view(std::string_view scene_json, RuntimeSceneView& out, ParseDiagnostics* diag) {
    LooseSceneDocument scene{};
    if (!parse_loose_scene_document(scene_json, scene, diag)) {
        return false;
    }
    out = make_runtime_scene_view(scene);
    return true;
}

bool has_selected_entity(const LooseSceneDocument& scene) noexcept {
    return !scene.selected_entity_is_null && !scene.selected_entity.empty();
}

const SceneEntity* find_entity(const LooseSceneDocument& scene, std::string_view entity_name) noexcept {
    if (entity_name.empty()) {
        return nullptr;
    }
    for (const SceneEntity& entity : scene.entities) {
        if (entity.name == entity_name) {
            return &entity;
        }
    }
    return nullptr;
}

const SceneEntity* find_selected_entity(const LooseSceneDocument& scene) noexcept {
    if (!has_selected_entity(scene)) {
        return nullptr;
    }
    return find_entity(scene, scene.selected_entity);
}

const SceneEntity* find_active_camera_entity(const LooseSceneDocument& scene) noexcept {
    if (scene.active_camera_is_null || scene.active_camera.empty()) {
        return nullptr;
    }
    const SceneEntity* entity = find_entity(scene, scene.active_camera);
    if (entity == nullptr || !entity->has_camera()) {
        return nullptr;
    }
    return entity;
}

const PrimitiveView* find_selected_primitive(const RuntimeSceneView& view) noexcept {
    for (const PrimitiveView& primitive : view.primitives) {
        if (primitive.selected) {
            return &primitive;
        }
    }
    return nullptr;
}

bool selected_entity_is_valid(const LooseSceneDocument& scene) noexcept {
    if (scene.selected_entity_is_null || scene.selected_entity.empty()) {
        return true;
    }
    return find_entity(scene, scene.selected_entity) != nullptr;
}

bool active_camera_is_valid(const LooseSceneDocument& scene) noexcept {
    if (scene.active_camera_is_null) {
        return true;
    }
    return find_active_camera_entity(scene) != nullptr;
}

void normalize_scene_document(LooseSceneDocument& scene) {
    scene.version = sanitize_scene_document_version(scene.version);
    if (scene.format.empty()) {
        scene.format = "psynder-gx-loose-scene-v1";
    }
    if (scene.name.empty()) {
        scene.name = "Untitled Scene";
    }
    if (scene.path.empty()) {
        scene.path = "memory://untitled.psyloose";
    }

    scene.editor_settings.snap_step = sanitize_snap_step(scene.editor_settings.snap_step);
    scene.editor_settings.snap_mode =
        snap_mode_from_token(snap_mode_token(scene.editor_settings.snap_mode));
    scene.editor_settings.transform_mode =
        transform_mode_from_token(transform_mode_token(scene.editor_settings.transform_mode));
    scene.editor_settings.render_debug =
        render_debug_mode_from_token(render_debug_mode_token(scene.editor_settings.render_debug));
    if (scene.editor_settings.has_viewport_camera) {
        sanitize_viewport_camera(scene.editor_settings.viewport_camera);
    }

    sanitize_environment(scene.environment_settings);

    for (SceneEntity& entity : scene.entities) {
        if (entity.has_player_start() || entity.has_player_controller()) {
            ensure_component(entity.components, "Transform");
        }
        sanitize_transform(entity.transform);
        sanitize_material(entity.material);
        sanitize_player_controller(entity.player_controller);
        entity.fov_y_deg = sanitize_fov_y_deg(entity.fov_y_deg);
        if (entity.primitive == PrimitiveKind::Unknown && entity.primitive_token.empty()) {
            entity.primitive = PrimitiveKind::None;
        } else if (entity.primitive != PrimitiveKind::Unknown) {
            entity.primitive_token = std::string{primitive_kind_token(entity.primitive)};
        }
    }

    if (!selected_entity_is_valid(scene)) {
        clear_selected_entity(scene);
    } else if (scene.selected_entity.empty()) {
        scene.selected_entity_is_null = true;
    }

    if (!active_camera_is_valid(scene)) {
        scene.active_camera.clear();
        scene.active_camera_is_null = true;
        for (const SceneEntity& entity : scene.entities) {
            if (!entity.has_camera() || entity.name.empty()) {
                continue;
            }
            scene.active_camera = entity.name;
            scene.active_camera_is_null = false;
            break;
        }
    }
}

void set_selected_entity(LooseSceneDocument& scene, std::string_view entity_name) {
    scene.selected_entity.assign(entity_name.data(), entity_name.size());
    scene.selected_entity_is_null = scene.selected_entity.empty();
}

void clear_selected_entity(LooseSceneDocument& scene) noexcept {
    scene.selected_entity.clear();
    scene.selected_entity_is_null = true;
}

void set_active_camera(LooseSceneDocument& scene, std::string_view entity_name) {
    scene.active_camera.assign(entity_name.data(), entity_name.size());
    scene.active_camera_is_null = scene.active_camera.empty();
    if (!active_camera_is_valid(scene)) {
        clear_active_camera(scene);
    }
}

void clear_active_camera(LooseSceneDocument& scene) noexcept {
    scene.active_camera.clear();
    scene.active_camera_is_null = true;
}

void set_snap_settings(LooseSceneDocument& scene, SnapMode mode, float snap_step) noexcept {
    scene.editor_settings.snap_mode = mode;
    scene.editor_settings.snap_step = sanitize_snap_step(snap_step);
}

void set_transform_mode(LooseSceneDocument& scene, TransformMode mode) noexcept {
    scene.editor_settings.transform_mode = transform_mode_from_token(transform_mode_token(mode));
}

void set_render_debug_mode(LooseSceneDocument& scene, RenderDebugMode mode) noexcept {
    scene.editor_settings.render_debug = render_debug_mode_from_token(render_debug_mode_token(mode));
}

void set_viewport_camera(LooseSceneDocument& scene, const ViewportCameraState& camera) noexcept {
    scene.editor_settings.viewport_camera = camera;
    sanitize_viewport_camera(scene.editor_settings.viewport_camera);
    scene.editor_settings.has_viewport_camera = true;
}

bool patch_selected_entity_json(std::string& document_json, std::string_view entity_name) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, nullptr)) {
        return false;
    }
    sanitize_json_authoring_state(root);
    if (entity_name.empty()) {
        set_object_member(root, "selected_entity", JsonValue{});
    } else {
        set_object_member(root, "selected_entity", make_string(entity_name));
    }
    sanitize_json_authoring_state(root);
    document_json = serialize_json_root(root);
    return true;
}

bool patch_active_camera_json(std::string& document_json, std::string_view entity_name) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, nullptr)) {
        return false;
    }
    sanitize_json_authoring_state(root);
    if (entity_name.empty()) {
        set_object_member(root, "active_camera", JsonValue{});
    } else {
        set_object_member(root, "active_camera", make_string(entity_name));
    }
    sanitize_json_authoring_state(root);
    document_json = serialize_json_root(root);
    return true;
}

bool patch_snap_settings_json(std::string& document_json, SnapMode mode, float snap_step) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, nullptr)) {
        return false;
    }
    sanitize_json_authoring_state(root);
    JsonValue& editor = ensure_object_member(root, "editorSettings");
    set_object_member(editor, "snap_mode", make_string(snap_mode_token(mode)));
    set_object_member(editor,
                      "snap_step",
                      make_number(static_cast<double>(sanitize_snap_step(snap_step))));
    document_json = serialize_json_root(root);
    return true;
}

bool patch_transform_mode_json(std::string& document_json, TransformMode mode) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, nullptr)) {
        return false;
    }
    sanitize_json_authoring_state(root);
    JsonValue& editor = ensure_object_member(root, "editorSettings");
    set_object_member(editor, "transform_mode", make_string(transform_mode_token(mode)));
    document_json = serialize_json_root(root);
    return true;
}

bool patch_render_debug_mode_json(std::string& document_json, RenderDebugMode mode) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, nullptr)) {
        return false;
    }
    sanitize_json_authoring_state(root);
    JsonValue& editor = ensure_object_member(root, "editorSettings");
    set_object_member(editor, "render_debug", make_string(render_debug_mode_token(mode)));
    document_json = serialize_json_root(root);
    return true;
}

bool patch_viewport_camera_json(std::string& document_json, const ViewportCameraState& camera) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, nullptr)) {
        return false;
    }
    sanitize_json_authoring_state(root);
    JsonValue& editor = ensure_object_member(root, "editorSettings");
    set_object_member(editor, "viewport_camera", viewport_camera_json(camera));
    document_json = serialize_json_root(root);
    return true;
}

bool parse_transform_mode_json(std::string_view document_json,
                               TransformMode& out,
                               ParseDiagnostics* diag) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, diag)) {
        return false;
    }
    out = TransformMode::Translate;
    if (const JsonValue* editor = member(root, "editorSettings");
        editor != nullptr && editor->kind == JsonKind::Object) {
        out = transform_mode_from_token(string_or(*editor, "transform_mode", "translate"));
    }
    return true;
}

bool parse_render_debug_mode_json(std::string_view document_json,
                                  RenderDebugMode& out,
                                  ParseDiagnostics* diag) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, diag)) {
        return false;
    }
    out = RenderDebugMode::Off;
    if (const JsonValue* editor = member(root, "editorSettings");
        editor != nullptr && editor->kind == JsonKind::Object) {
        out = render_debug_mode_from_token(string_or(*editor, "render_debug", "off"));
    }
    return true;
}

bool parse_viewport_camera_json(std::string_view document_json,
                                ViewportCameraState& out,
                                ParseDiagnostics* diag) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, diag)) {
        return false;
    }
    out = {};
    if (const JsonValue* editor = member(root, "editorSettings");
        editor != nullptr && editor->kind == JsonKind::Object) {
        if (const JsonValue* camera = member(*editor, "viewport_camera");
            camera != nullptr && camera->kind == JsonKind::Object) {
            out = viewport_camera_or(*camera, out);
            return true;
        }
    }
    sanitize_viewport_camera(out);
    return true;
}

bool ensure_authoring_state_json(std::string& document_json) {
    JsonValue root{};
    if (!parse_json_root(document_json, root, nullptr)) {
        return false;
    }
    sanitize_json_authoring_state(root);
    document_json = serialize_json_root(root);
    return true;
}

}  // namespace psynder::editor::scene_authoring
