// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "math/Math.h"

namespace psynder::editor::scene_authoring {

enum class PrimitiveKind {
    None,
    Cube,
    Sphere,
    Plane,
    Capsule,
    Unknown,
};

enum class SnapMode {
    Off,
    Grid,
};

enum class TransformMode {
    Translate,
    Rotate,
    Scale,
};

enum class RenderDebugMode {
    Off,
    Depth,
};

inline constexpr int k_current_scene_document_version = 1;

struct Transform {
    math::Vec3 position{0.0f, 0.0f, 0.0f};
    math::Vec3 rotation_euler_deg{0.0f, 0.0f, 0.0f};
    math::Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct MaterialSettings {
    math::Vec3 albedo{0.78f, 0.78f, 0.78f};
    float roughness = 0.55f;
    float metallic = 0.0f;
    math::Vec3 emissive{0.0f, 0.0f, 0.0f};
    float emissive_intensity = 0.0f;
};

struct PlayerControllerSettings {
    float walk_speed = 4.6f;
    float run_speed = 7.0f;
    float jump_velocity = 5.2f;
    float mouse_sensitivity = 0.10f;
    float capsule_radius = 0.32f;
    float capsule_height = 1.75f;
};

struct SceneEntity {
    std::string name;
    std::vector<std::string> components;
    PrimitiveKind primitive = PrimitiveKind::None;
    std::string primitive_token;
    Transform transform;
    MaterialSettings material;
    PlayerControllerSettings player_controller;
    float fov_y_deg = 70.0f;
    bool visible = true;

    [[nodiscard]] bool has_camera() const noexcept;
    [[nodiscard]] bool has_primitive() const noexcept;
    [[nodiscard]] bool has_player_start() const noexcept;
    [[nodiscard]] bool has_player_controller() const noexcept;
    [[nodiscard]] bool selected(std::string_view selected_entity) const noexcept;
};

struct ViewportCameraState {
    math::Vec3 position{0.0f, 1.4f, 6.0f};
    math::Vec3 rotation_euler_deg{-10.0f, 180.0f, 0.0f};
    float fov_y_deg = 70.0f;
};

struct EditorSettings {
    bool grid_visible = true;
    SnapMode snap_mode = SnapMode::Off;
    float snap_step = 1.0f;
    TransformMode transform_mode = TransformMode::Translate;
    RenderDebugMode render_debug = RenderDebugMode::Off;
    ViewportCameraState viewport_camera;
    bool has_viewport_camera = false;
};

struct CloudSettings {
    float coverage = 0.42f;
    float density = 0.55f;
    float speed = 0.035f;
    float height_m = 1800.0f;
};

struct EnvironmentSettings {
    float clear_color[4] = {0.018f, 0.027f, 0.050f, 1.0f};
    std::string sky_mode;
    float time_of_day_hours = 14.0f;
    math::Vec3 sun_direction{0.25f, -0.78f, 0.57f};
    math::Vec3 sun_color{1.0f, 0.93f, 0.82f};
    float sun_intensity = 4.0f;
    math::Vec3 ambient_color{0.20f, 0.25f, 0.35f};
    float ambient_intensity = 0.35f;
    float exposure = 1.0f;
    CloudSettings clouds;
};

struct LooseSceneDocument {
    int version = k_current_scene_document_version;
    std::string format = "psynder-gx-loose-scene-v1";
    std::string name = "Untitled Scene";
    std::string path = "memory://untitled.psyloose";
    std::string active_camera;
    bool active_camera_is_null = false;
    std::string selected_entity;
    bool selected_entity_is_null = true;
    EditorSettings editor_settings;
    EnvironmentSettings environment_settings;
    std::vector<SceneEntity> entities;
};

struct ParseDiagnostics {
    bool ok = true;
    std::size_t offset = 0;
    std::string message;
};

struct CameraView {
    std::string name;
    Transform transform;
    float fov_y_deg = 70.0f;
    bool active = false;
};

struct PrimitiveView {
    std::string name;
    PrimitiveKind kind = PrimitiveKind::Cube;
    std::string primitive_token;
    Transform transform;
    MaterialSettings material;
    bool visible = true;
    bool selected = false;
};

struct PlayerStartView {
    std::string name;
    Transform transform;
    bool selected = false;
};

struct PlayerControllerView {
    std::string name;
    Transform transform;
    PlayerControllerSettings settings;
    bool selected = false;
};

struct RuntimeSceneView {
    float clear_color[4] = {0.018f, 0.027f, 0.050f, 1.0f};
    EnvironmentSettings environment;
    bool grid_visible = true;
    SnapMode snap_mode = SnapMode::Off;
    float snap_step = 1.0f;
    TransformMode transform_mode = TransformMode::Translate;
    RenderDebugMode render_debug = RenderDebugMode::Off;
    bool has_active_camera = false;
    CameraView active_camera;
    std::vector<PrimitiveView> primitives;
    std::vector<PlayerStartView> player_starts;
    std::vector<PlayerControllerView> player_controllers;
};

[[nodiscard]] PrimitiveKind primitive_kind_from_token(std::string_view token) noexcept;
[[nodiscard]] std::string_view primitive_kind_token(PrimitiveKind kind) noexcept;
[[nodiscard]] SnapMode snap_mode_from_token(std::string_view token) noexcept;
[[nodiscard]] std::string_view snap_mode_token(SnapMode mode) noexcept;
[[nodiscard]] TransformMode transform_mode_from_token(std::string_view token) noexcept;
[[nodiscard]] std::string_view transform_mode_token(TransformMode mode) noexcept;
[[nodiscard]] RenderDebugMode render_debug_mode_from_token(std::string_view token) noexcept;
[[nodiscard]] std::string_view render_debug_mode_token(RenderDebugMode mode) noexcept;
[[nodiscard]] int sanitize_scene_document_version(int version) noexcept;
[[nodiscard]] float sanitize_snap_step(float snap_step) noexcept;
[[nodiscard]] float sanitize_fov_y_deg(float fov_y_deg) noexcept;
void sanitize_clear_color(float clear_color[4]) noexcept;
void sanitize_viewport_camera(ViewportCameraState& camera) noexcept;
void sanitize_transform(Transform& transform) noexcept;
void sanitize_material(MaterialSettings& material) noexcept;
void sanitize_player_controller(PlayerControllerSettings& player_controller) noexcept;
void sanitize_clouds(CloudSettings& clouds) noexcept;
void sanitize_environment(EnvironmentSettings& environment) noexcept;

[[nodiscard]] LooseSceneDocument make_default_scene_document();

[[nodiscard]] bool parse_loose_scene_document(std::string_view scene_json,
                                              LooseSceneDocument& out,
                                              ParseDiagnostics* diag = nullptr);

[[nodiscard]] std::string serialize_loose_scene_document(const LooseSceneDocument& scene);

[[nodiscard]] RuntimeSceneView make_runtime_scene_view(const LooseSceneDocument& scene);
[[nodiscard]] bool parse_runtime_scene_view(std::string_view scene_json,
                                            RuntimeSceneView& out,
                                            ParseDiagnostics* diag = nullptr);

[[nodiscard]] bool has_selected_entity(const LooseSceneDocument& scene) noexcept;
[[nodiscard]] const SceneEntity* find_entity(const LooseSceneDocument& scene,
                                             std::string_view entity_name) noexcept;
[[nodiscard]] const SceneEntity* find_selected_entity(const LooseSceneDocument& scene) noexcept;
[[nodiscard]] const SceneEntity* find_active_camera_entity(const LooseSceneDocument& scene) noexcept;
[[nodiscard]] const PrimitiveView* find_selected_primitive(const RuntimeSceneView& view) noexcept;
[[nodiscard]] bool selected_entity_is_valid(const LooseSceneDocument& scene) noexcept;
[[nodiscard]] bool active_camera_is_valid(const LooseSceneDocument& scene) noexcept;

void normalize_scene_document(LooseSceneDocument& scene);
void set_selected_entity(LooseSceneDocument& scene, std::string_view entity_name);
void clear_selected_entity(LooseSceneDocument& scene) noexcept;
void set_active_camera(LooseSceneDocument& scene, std::string_view entity_name);
void clear_active_camera(LooseSceneDocument& scene) noexcept;
void set_snap_settings(LooseSceneDocument& scene, SnapMode mode, float snap_step) noexcept;
void set_transform_mode(LooseSceneDocument& scene, TransformMode mode) noexcept;
void set_render_debug_mode(LooseSceneDocument& scene, RenderDebugMode mode) noexcept;
void set_viewport_camera(LooseSceneDocument& scene, const ViewportCameraState& camera) noexcept;

[[nodiscard]] bool patch_selected_entity_json(std::string& document_json, std::string_view entity_name);
[[nodiscard]] bool patch_active_camera_json(std::string& document_json, std::string_view entity_name);
[[nodiscard]] bool patch_snap_settings_json(std::string& document_json, SnapMode mode, float snap_step);
[[nodiscard]] bool patch_transform_mode_json(std::string& document_json, TransformMode mode);
[[nodiscard]] bool patch_render_debug_mode_json(std::string& document_json, RenderDebugMode mode);
[[nodiscard]] bool patch_viewport_camera_json(std::string& document_json,
                                              const ViewportCameraState& camera);
[[nodiscard]] bool parse_transform_mode_json(std::string_view document_json,
                                             TransformMode& out,
                                             ParseDiagnostics* diag = nullptr);
[[nodiscard]] bool parse_render_debug_mode_json(std::string_view document_json,
                                                RenderDebugMode& out,
                                                ParseDiagnostics* diag = nullptr);
[[nodiscard]] bool parse_viewport_camera_json(std::string_view document_json,
                                              ViewportCameraState& out,
                                              ParseDiagnostics* diag = nullptr);
[[nodiscard]] bool ensure_authoring_state_json(std::string& document_json);

}  // namespace psynder::editor::scene_authoring
