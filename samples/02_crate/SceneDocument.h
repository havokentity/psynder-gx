// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "editor/scene_authoring/SceneDocument.h"
#include "math/Math.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psynder::sample02 {

enum class ScenePrimitiveKind {
    Cube,
    Sphere,
    Plane,
};

struct SceneSolidMaterial {
    psynder::math::Vec3 albedo{0.76f, 0.82f, 0.88f};
    psynder::math::Vec3 emissive{0.0f, 0.0f, 0.0f};
    float roughness = 0.65f;
    float metallic = 0.0f;
    float emissive_intensity = 0.0f;
};

struct ScenePrimitive {
    std::string name;
    ScenePrimitiveKind kind = ScenePrimitiveKind::Cube;
    psynder::math::Vec3 position{0.0f, 0.5f, 0.0f};
    psynder::math::Vec3 rotation_euler_deg{0.0f, 0.0f, 0.0f};
    psynder::math::Vec3 scale{1.0f, 1.0f, 1.0f};
    SceneSolidMaterial material{};
    bool visible = true;
    bool selected = false;
};

struct SceneCamera {
    std::string name;
    psynder::math::Vec3 position{0.0f, 1.4f, 4.2f};
    psynder::math::Vec3 rotation_euler_deg{-8.0f, 180.0f, 0.0f};
    float fov_y_deg = 70.0f;
    bool active = false;
};

struct ScenePlayerStart {
    std::string name;
    psynder::math::Vec3 position{0.0f, 0.05f, 0.0f};
    psynder::math::Vec3 rotation_euler_deg{0.0f, 180.0f, 0.0f};
    bool selected = false;
};

struct ScenePlayerController {
    std::string name;
    psynder::math::Vec3 position{0.0f, 0.05f, 0.0f};
    psynder::math::Vec3 rotation_euler_deg{0.0f, 180.0f, 0.0f};
    float walk_speed = 4.6f;
    float run_speed = 7.0f;
    float jump_velocity = 5.2f;
    float mouse_sensitivity = 0.10f;
    float capsule_radius = 0.32f;
    float capsule_height = 1.75f;
    bool selected = false;
};

struct SceneEnvironment {
    std::string sky_mode = "sdf-boot-field";
    psynder::math::Vec3 sun_direction{0.32f, -0.72f, 0.54f};
    psynder::math::Vec3 sun_color{1.0f, 0.93f, 0.82f};
    float sun_intensity_lux = 85000.0f;
    float time_of_day_hours = 15.25f;
    psynder::math::Vec3 ambient_color{0.35f, 0.43f, 0.56f};
    float ambient_intensity = 0.22f;
    float exposure = 1.0f;
    float cloud_coverage = 0.38f;
    float cloud_density = 0.62f;
    float cloud_speed = 0.035f;
    float cloud_height_m = 1800.0f;
    float cloud_thickness_m = 950.0f;
};

struct ParsedSceneDocument {
    float clear_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    SceneEnvironment environment{};
    bool editor_grid_visible = true;
    bool has_active_camera = false;
    SceneCamera camera;
    std::vector<ScenePlayerStart> player_starts;
    std::vector<ScenePlayerController> player_controllers;
    std::vector<ScenePrimitive> primitives;
};

namespace detail {

inline float clamp01(float value) noexcept {
    return std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
}

inline float sanitize_positive(float value, float fallback, float max_value) noexcept {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, 0.0f, max_value);
}

inline bool loose_scene_has_key(std::string_view text, std::string_view key) {
    std::string quoted;
    quoted.reserve(key.size() + 2u);
    quoted.push_back('"');
    quoted.append(key.data(), key.size());
    quoted.push_back('"');
    return text.find(quoted) != std::string_view::npos;
}

inline std::size_t find_json_matching(std::string_view text,
                                      std::size_t open_pos,
                                      char open_char,
                                      char close_char) noexcept {
    if (open_pos == std::string_view::npos ||
        open_pos >= text.size() ||
        text[open_pos] != open_char) {
        return std::string_view::npos;
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = open_pos; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string && c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (c == open_char) {
            ++depth;
        } else if (c == close_char) {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

inline std::size_t find_json_field_value_begin(std::string_view text,
                                               std::string_view key,
                                               std::size_t search_from = 0) noexcept {
    const std::string needle = "\"" + std::string{key} + "\"";
    const std::size_t key_pos = text.find(needle, search_from);
    if (key_pos == std::string_view::npos) {
        return std::string_view::npos;
    }
    const std::size_t colon = text.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) {
        return std::string_view::npos;
    }
    std::size_t value_begin = colon + 1u;
    while (value_begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[value_begin]))) {
        ++value_begin;
    }
    return value_begin;
}

inline bool find_json_object_field(std::string_view text,
                                   std::string_view key,
                                   std::size_t& out_begin,
                                   std::size_t& out_end) noexcept {
    const std::size_t begin = find_json_field_value_begin(text, key);
    if (begin == std::string_view::npos || begin >= text.size() || text[begin] != '{') {
        return false;
    }
    const std::size_t close = find_json_matching(text, begin, '{', '}');
    if (close == std::string_view::npos) {
        return false;
    }
    out_begin = begin;
    out_end = close + 1u;
    return true;
}

inline std::string parse_json_string_field(std::string_view text, std::string_view key) {
    const std::size_t begin = find_json_field_value_begin(text, key);
    if (begin == std::string_view::npos || begin >= text.size() || text[begin] != '"') {
        return {};
    }
    bool escaped = false;
    std::string out;
    for (std::size_t i = begin + 1u; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            out.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            return out;
        }
        out.push_back(c);
    }
    return {};
}

inline bool parse_json_float_field(std::string_view text,
                                   std::string_view key,
                                   float& out) {
    const std::size_t begin = find_json_field_value_begin(text, key);
    if (begin == std::string_view::npos) {
        return false;
    }
    std::string scratch{text.substr(begin, std::min<std::size_t>(64u, text.size() - begin))};
    char* end = nullptr;
    const float parsed = std::strtof(scratch.c_str(), &end);
    if (end == scratch.c_str()) {
        return false;
    }
    out = parsed;
    return true;
}

inline bool parse_json_vec3_field(std::string_view text,
                                  std::string_view key,
                                  psynder::math::Vec3& out) {
    const std::size_t begin = find_json_field_value_begin(text, key);
    if (begin == std::string_view::npos || begin >= text.size() || text[begin] != '[') {
        return false;
    }
    const std::size_t end = find_json_matching(text, begin, '[', ']');
    if (end == std::string_view::npos || end <= begin) {
        return false;
    }
    std::string scratch{text.substr(begin + 1u, end - begin - 1u)};
    const char* cursor = scratch.c_str();
    char* parsed_end = nullptr;
    float values[3]{};
    for (float& value : values) {
        while (*cursor != '\0' &&
               (std::isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',')) {
            ++cursor;
        }
        value = std::strtof(cursor, &parsed_end);
        if (parsed_end == cursor) {
            return false;
        }
        cursor = parsed_end;
    }
    out = {values[0], values[1], values[2]};
    return true;
}

inline psynder::math::Vec3 sanitize_color3(psynder::math::Vec3 value,
                                           psynder::math::Vec3 fallback) noexcept {
    return {
        std::isfinite(value.x) ? std::clamp(value.x, 0.0f, 32.0f) : fallback.x,
        std::isfinite(value.y) ? std::clamp(value.y, 0.0f, 32.0f) : fallback.y,
        std::isfinite(value.z) ? std::clamp(value.z, 0.0f, 32.0f) : fallback.z,
    };
}

inline SceneSolidMaterial default_material_for_kind(ScenePrimitiveKind kind) noexcept {
    SceneSolidMaterial material{};
    switch (kind) {
        case ScenePrimitiveKind::Sphere:
            material.albedo = {0.48f, 0.72f, 1.0f};
            material.roughness = 0.42f;
            material.metallic = 0.05f;
            break;
        case ScenePrimitiveKind::Plane:
            material.albedo = {0.42f, 0.48f, 0.50f};
            material.roughness = 0.85f;
            material.metallic = 0.0f;
            break;
        case ScenePrimitiveKind::Cube:
            material.albedo = {0.86f, 0.80f, 0.62f};
            material.roughness = 0.58f;
            material.metallic = 0.0f;
            break;
    }
    return material;
}

inline SceneSolidMaterial parse_solid_material(std::string_view entity_object,
                                               ScenePrimitiveKind kind) {
    SceneSolidMaterial material = default_material_for_kind(kind);
    std::size_t begin = 0;
    std::size_t end = 0;
    if (!find_json_object_field(entity_object, "material", begin, end)) {
        return material;
    }
    const std::string_view object = entity_object.substr(begin, end - begin);
    psynder::math::Vec3 color = material.albedo;
    if (parse_json_vec3_field(object, "albedo", color)) {
        material.albedo = sanitize_color3(color, material.albedo);
    }
    color = material.emissive;
    if (parse_json_vec3_field(object, "emissive", color)) {
        material.emissive = sanitize_color3(color, material.emissive);
    }
    (void)parse_json_float_field(object, "roughness", material.roughness);
    (void)parse_json_float_field(object, "metallic", material.metallic);
    (void)parse_json_float_field(object, "emissive_intensity", material.emissive_intensity);
    material.roughness = std::clamp(std::isfinite(material.roughness) ? material.roughness : 0.65f,
                                    0.02f,
                                    1.0f);
    material.metallic = clamp01(material.metallic);
    material.emissive_intensity = sanitize_positive(material.emissive_intensity, 0.0f, 64.0f);
    return material;
}

inline bool find_scene_entity_object(std::string_view document_json,
                                     std::string_view entity_name,
                                     std::size_t& out_begin,
                                     std::size_t& out_end) noexcept {
    const std::size_t entities_begin = find_json_field_value_begin(document_json, "entities");
    if (entities_begin == std::string_view::npos ||
        entities_begin >= document_json.size() ||
        document_json[entities_begin] != '[') {
        return false;
    }
    const std::size_t entities_end = find_json_matching(document_json, entities_begin, '[', ']');
    if (entities_end == std::string_view::npos) {
        return false;
    }
    std::size_t cursor = entities_begin + 1u;
    while (cursor < entities_end) {
        const std::size_t object_begin = document_json.find('{', cursor);
        if (object_begin == std::string_view::npos || object_begin >= entities_end) {
            break;
        }
        const std::size_t object_close = find_json_matching(document_json, object_begin, '{', '}');
        if (object_close == std::string_view::npos || object_close > entities_end) {
            break;
        }
        const std::string_view object =
            document_json.substr(object_begin, object_close - object_begin + 1u);
        if (parse_json_string_field(object, "name") == entity_name) {
            out_begin = object_begin;
            out_end = object_close + 1u;
            return true;
        }
        cursor = object_close + 1u;
    }
    return false;
}

inline psynder::math::Vec3 time_of_day_sun_direction(float hours) noexcept {
    const float normalized = std::clamp(hours / 24.0f, 0.0f, 1.0f);
    const float angle = (normalized - 0.25f) * psynder::math::kTwoPi;
    const float y = -std::max(std::sin(angle), 0.08f);
    const float x = 0.42f * std::cos(angle);
    const float z = 0.72f;
    return psynder::math::normalize({x, y, z});
}

inline void parse_environment_settings(std::string_view scene_text,
                                       ParsedSceneDocument& out) {
    std::size_t begin = 0;
    std::size_t end = 0;
    if (!find_json_object_field(scene_text, "environmentSettings", begin, end)) {
        return;
    }
    const std::string_view env = scene_text.substr(begin, end - begin);
    if (std::string sky_mode = parse_json_string_field(env, "sky_mode"); !sky_mode.empty()) {
        out.environment.sky_mode = std::move(sky_mode);
    }
    std::size_t object_begin = 0;
    std::size_t object_end = 0;
    if (find_json_object_field(env, "sun", object_begin, object_end)) {
        const std::string_view sun = env.substr(object_begin, object_end - object_begin);
        (void)parse_json_float_field(sun, "time_of_day_hours", out.environment.time_of_day_hours);
        (void)parse_json_vec3_field(sun, "direction", out.environment.sun_direction);
        (void)parse_json_vec3_field(sun, "color", out.environment.sun_color);
        (void)parse_json_float_field(sun, "intensity_lux", out.environment.sun_intensity_lux);
    }
    if (find_json_object_field(env, "lighting", object_begin, object_end)) {
        const std::string_view lighting = env.substr(object_begin, object_end - object_begin);
        (void)parse_json_vec3_field(lighting, "ambient_color", out.environment.ambient_color);
        (void)parse_json_float_field(lighting, "ambient_intensity", out.environment.ambient_intensity);
        (void)parse_json_float_field(lighting, "exposure", out.environment.exposure);
    }
    if (find_json_object_field(env, "clouds", object_begin, object_end)) {
        const std::string_view clouds = env.substr(object_begin, object_end - object_begin);
        (void)parse_json_float_field(clouds, "coverage", out.environment.cloud_coverage);
        (void)parse_json_float_field(clouds, "density", out.environment.cloud_density);
        (void)parse_json_float_field(clouds, "speed", out.environment.cloud_speed);
        (void)parse_json_float_field(clouds, "height_m", out.environment.cloud_height_m);
        (void)parse_json_float_field(clouds, "thickness_m", out.environment.cloud_thickness_m);
    }
    out.environment.time_of_day_hours =
        std::clamp(std::isfinite(out.environment.time_of_day_hours)
                       ? out.environment.time_of_day_hours
                       : 15.25f,
                   0.0f,
                   24.0f);
    if (psynder::math::length(out.environment.sun_direction) <= 0.001f) {
        out.environment.sun_direction = time_of_day_sun_direction(out.environment.time_of_day_hours);
    } else {
        out.environment.sun_direction = psynder::math::normalize(out.environment.sun_direction);
    }
    out.environment.sun_color =
        sanitize_color3(out.environment.sun_color, {1.0f, 0.93f, 0.82f});
    out.environment.sun_intensity_lux =
        sanitize_positive(out.environment.sun_intensity_lux, 85000.0f, 140000.0f);
    out.environment.ambient_color =
        sanitize_color3(out.environment.ambient_color, {0.35f, 0.43f, 0.56f});
    out.environment.ambient_intensity =
        sanitize_positive(out.environment.ambient_intensity, 0.22f, 8.0f);
    out.environment.exposure = std::clamp(std::isfinite(out.environment.exposure)
                                              ? out.environment.exposure
                                              : 1.0f,
                                          0.05f,
                                          8.0f);
    out.environment.cloud_coverage = clamp01(out.environment.cloud_coverage);
    out.environment.cloud_density =
        std::clamp(std::isfinite(out.environment.cloud_density)
                       ? out.environment.cloud_density
                       : 0.62f,
                   0.0f,
                   4.0f);
    out.environment.cloud_speed =
        std::clamp(std::isfinite(out.environment.cloud_speed)
                       ? out.environment.cloud_speed
                       : 0.035f,
                   -2.0f,
                   2.0f);
    out.environment.cloud_height_m =
        sanitize_positive(out.environment.cloud_height_m, 1800.0f, 20000.0f);
    out.environment.cloud_thickness_m =
        sanitize_positive(out.environment.cloud_thickness_m, 950.0f, 12000.0f);
}

inline ScenePrimitiveKind sample_kind_from_authoring(
    psynder::editor::scene_authoring::PrimitiveKind kind) noexcept {
    namespace scene = psynder::editor::scene_authoring;
    switch (kind) {
        case scene::PrimitiveKind::Sphere: return ScenePrimitiveKind::Sphere;
        case scene::PrimitiveKind::Plane:  return ScenePrimitiveKind::Plane;
        case scene::PrimitiveKind::Cube:
        case scene::PrimitiveKind::Capsule:
        case scene::PrimitiveKind::Unknown:
        case scene::PrimitiveKind::None:
            break;
    }
    return ScenePrimitiveKind::Cube;
}

inline void apply_camera_view(ParsedSceneDocument& out,
                              const psynder::editor::scene_authoring::CameraView& camera) {
    out.camera.name = camera.name;
    out.camera.position = camera.transform.position;
    out.camera.rotation_euler_deg = camera.transform.rotation_euler_deg;
    out.camera.fov_y_deg = std::clamp(camera.fov_y_deg, 1.0f, 179.0f);
    out.camera.active = true;
    out.has_active_camera = true;
}

inline bool choose_first_camera_if_active_missing(
    const psynder::editor::scene_authoring::LooseSceneDocument& scene,
    ParsedSceneDocument& out) {
    for (const psynder::editor::scene_authoring::SceneEntity& entity : scene.entities) {
        if (!entity.has_camera()) {
            continue;
        }
        psynder::editor::scene_authoring::CameraView camera{};
        camera.name = entity.name;
        camera.transform = entity.transform;
        camera.fov_y_deg = entity.fov_y_deg;
        camera.active = true;
        apply_camera_view(out, camera);
        return true;
    }
    return false;
}

} // namespace detail

inline ParsedSceneDocument parse_loose_scene_document(std::string_view scene_text) {
    namespace scene = psynder::editor::scene_authoring;

    ParsedSceneDocument out{};
    scene::LooseSceneDocument parsed = scene::make_default_scene_document();
    scene::ParseDiagnostics diag{};
    if (!scene::parse_loose_scene_document(scene_text, parsed, &diag)) {
        return out;
    }

    const scene::RuntimeSceneView runtime = scene::make_runtime_scene_view(parsed);
    std::copy(std::begin(runtime.clear_color),
              std::end(runtime.clear_color),
              std::begin(out.clear_rgba));
    out.editor_grid_visible = runtime.grid_visible;
    detail::parse_environment_settings(scene_text, out);

    if (runtime.has_active_camera) {
        detail::apply_camera_view(out, runtime.active_camera);
    } else if (!detail::loose_scene_has_key(scene_text, "active_camera") &&
               !parsed.active_camera_is_null) {
        (void)detail::choose_first_camera_if_active_missing(parsed, out);
    }

    out.primitives.reserve(runtime.primitives.size());
    out.player_starts.reserve(runtime.player_starts.size());
    out.player_controllers.reserve(runtime.player_controllers.size());
    for (const scene::PlayerStartView& player_start_view : runtime.player_starts) {
        if (player_start_view.name.empty()) {
            continue;
        }
        ScenePlayerStart player_start{};
        player_start.name = player_start_view.name;
        player_start.position = player_start_view.transform.position;
        player_start.rotation_euler_deg = player_start_view.transform.rotation_euler_deg;
        player_start.selected = player_start_view.selected;
        out.player_starts.push_back(std::move(player_start));
    }
    for (const scene::PlayerControllerView& controller_view : runtime.player_controllers) {
        if (controller_view.name.empty()) {
            continue;
        }
        ScenePlayerController controller{};
        controller.name = controller_view.name;
        controller.position = controller_view.transform.position;
        controller.rotation_euler_deg = controller_view.transform.rotation_euler_deg;
        controller.walk_speed = controller_view.settings.walk_speed;
        controller.run_speed = controller_view.settings.run_speed;
        controller.jump_velocity = controller_view.settings.jump_velocity;
        controller.mouse_sensitivity = controller_view.settings.mouse_sensitivity;
        controller.capsule_radius = controller_view.settings.capsule_radius;
        controller.capsule_height = controller_view.settings.capsule_height;
        controller.selected = controller_view.selected;
        out.player_controllers.push_back(std::move(controller));
    }

    for (const scene::PrimitiveView& primitive_view : runtime.primitives) {
        if (!primitive_view.visible || primitive_view.name.empty() ||
            primitive_view.kind == scene::PrimitiveKind::None ||
            primitive_view.kind == scene::PrimitiveKind::Unknown) {
            continue;
        }
        ScenePrimitive primitive{};
        primitive.name = primitive_view.name;
        primitive.kind = detail::sample_kind_from_authoring(primitive_view.kind);
        primitive.material = detail::default_material_for_kind(primitive.kind);
        std::size_t entity_begin = 0;
        std::size_t entity_end = 0;
        if (detail::find_scene_entity_object(scene_text,
                                             primitive.name,
                                             entity_begin,
                                             entity_end)) {
            const std::string_view entity_object =
                scene_text.substr(entity_begin, entity_end - entity_begin);
            primitive.material =
                detail::parse_solid_material(entity_object, primitive.kind);
        }
        primitive.position = primitive_view.transform.position;
        primitive.rotation_euler_deg = primitive_view.transform.rotation_euler_deg;
        primitive.scale = primitive_view.transform.scale;
        primitive.visible = primitive_view.visible;
        primitive.selected = primitive_view.selected;
        out.primitives.push_back(std::move(primitive));
    }
    return out;
}

} // namespace psynder::sample02
