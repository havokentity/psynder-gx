// SPDX-License-Identifier: MIT OR Apache-2.0
//
// samples/02_crate/main.cpp
//
// The one small integration sample for the next Psynder-GX push. It should
// stay boring and load-bearing: one crate, one camera, one engine loop.
// New systems graduate through this sample only when their unit tests already
// prove the local contract.

#include "camera/Camera.h"
#include "../combat/Combat.h"
#include "SceneDocument.h"
#include "SceneImport.h"
#include "core/console/Console.h"
#include "core/console/RuntimeConsole.h"
#include "core/Types.h"
#include "editor/ipc/ConsoleBridge.h"
#include "editor/ipc/Ipc.h"
#include "asset/Vfs.h"
#include "gpu/PublicGpu.h"
#include "jobs/JobSystem.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "physics/agents/AgentComponents.h"
#include "physics/agents/AgentSystem.h"
#include "physics/core/CharacterController.h"
#include "physics/core/EcsCharacterBridge.h"
#include "physics/destruction/FractureSpawn.h"
#include "scene/GxComponents.h"
#include "scene/Replay.h"
#include "scene/SceneComponents.h"
#include "scene/World.h"
#include "script/Script.h"
#include "shader/PublicShader.h"
#include "ui/imm/RuntimeConsoleGpu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef PSYNDER_GX_APP_LOG_PREFIX
#  define PSYNDER_GX_APP_LOG_PREFIX "sample_02_crate"
#endif
#ifndef PSYNDER_GX_APP_WINDOW_TITLE
#  define PSYNDER_GX_APP_WINDOW_TITLE "Psynder-GX | sample_02_crate"
#endif
#ifndef PSYNDER_GX_APP_PASS_LABEL
#  define PSYNDER_GX_APP_PASS_LABEL "crate integration"
#endif
#ifndef PSYNDER_GX_APP_ENTITY_LABEL
#  define PSYNDER_GX_APP_ENTITY_LABEL "sample_entities"
#endif
#ifndef PSYNDER_GX_EDITOR_IPC_DEFAULT_PORT
#  define PSYNDER_GX_EDITOR_IPC_DEFAULT_PORT 7655
#endif

#if defined(PSYNDER_GX_PLATFORM_MACOS)
#  include "platform/macos/PublicPlatformMacos.h"
#  include <spawn.h>
#elif defined(PSYNDER_GX_PLATFORM_WIN32)
#  include "platform/Platform.h"
#  include "platform/win32/Win32Window.h"
#elif defined(PSYNDER_GX_PLATFORM_LINUX)
#  include "platform/Platform.h"
#  include "platform/linux/PublicPlatformLinux.h"
#endif

#if defined(PSYNDER_GX_PLATFORM_MACOS)
extern char** environ;
#endif

namespace {

struct Args {
    int  smoke_frames = 0;
    bool quiet        = false;
    std::vector<std::pair<std::string, std::string>> mounts;
    std::string manifest = "game:/manifest.psycooked";
};

enum class PlayerBootRenderState {
    NoSceneLoaded,
    NoCameraRendering,
    SceneRendering,
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

// Play-mode input recording state. Recording captures one InputFrame per fixed
// tick during live play; Replaying drives the sim from a recorded stream instead
// of live input (the `replay` console command), reproducing the session.
enum class ReplayMode {
    Off,
    Recording,
    Replaying,
};

using ScenePrimitiveKind = psynder::sample02::ScenePrimitiveKind;
using ScenePrimitive = psynder::sample02::ScenePrimitive;
using SceneCamera = psynder::sample02::SceneCamera;
using SceneEnvironment = psynder::sample02::SceneEnvironment;
using ScenePlayerStart = psynder::sample02::ScenePlayerStart;
using ScenePlayerController = psynder::sample02::ScenePlayerController;
namespace phys = psynder::physics;
namespace character_spine = psynder::physics::character_spine;

// Debug-draw toggles, flipped by the `debugdraw` console command and read by the
// play-mode render pass. The sample is single-instance, so file-scope state is
// fine. `colliders` overlays each collision volume as a bright per-type shell;
// `flow` shows each agent's heading (along its velocity) + its steering target.
namespace {
bool g_debug_draw_colliders = false;
bool g_debug_draw_flow = false;
}  // namespace

struct EditorPickRay {
    psynder::math::Vec3 origin{};
    psynder::math::Vec3 dir{0.0f, 0.0f, -1.0f};
};

struct EditorCameraState {
    psynder::math::Vec3 position{0.0f, 1.4f, 4.2f};
    psynder::math::Vec3 rotation_euler_deg{-8.0f, 180.0f, 0.0f};
    float fov_y_deg = 70.0f;
    bool initialized = false;
    bool user_override = false;
    bool orbiting_prev = false;
    std::string source_camera_name;
};

struct ViewportCameraState {
    psynder::math::Vec3 position{0.0f, 1.4f, 4.2f};
    psynder::math::Vec3 rotation_euler_deg{-8.0f, 180.0f, 0.0f};
    float fov_y_deg = 70.0f;
};

struct ViewportCameraTuning {
    float focus_distance = 6.0f;
    float wheel_distance = 6.0f;
    bool has_focus = false;
};

enum class GizmoAxis {
    None,
    X,
    Y,
    Z,
    PlaneXY,
    PlaneXZ,
    PlaneYZ,
};

struct GizmoDragState {
    bool active = false;
    TransformMode mode = TransformMode::Translate;
    GizmoAxis axis = GizmoAxis::None;
    std::string entity_name;
    bool allow_scale = true;
    psynder::math::Vec3 start_position{};
    psynder::math::Vec3 start_rotation_euler_deg{};
    psynder::math::Vec3 start_scale{1.0f, 1.0f, 1.0f};
    psynder::math::Vec3 start_plane_point{};
    psynder::math::Vec3 plane_normal{};
    psynder::math::Vec3 plane_u{};
    psynder::math::Vec3 plane_v{};
    float start_axis_param = 0.0f;
    float start_mouse_x = 0.0f;
    float start_mouse_y = 0.0f;
};

struct SelectedSceneTransform {
    std::string name;
    psynder::math::Vec3 position{};
    psynder::math::Vec3 rotation_euler_deg{};
    psynder::math::Vec3 scale{1.0f, 1.0f, 1.0f};
    bool allow_scale = true;
};

struct PlayerBootState {
    PlayerBootRenderState render_state = PlayerBootRenderState::NoSceneLoaded;
    std::string manifest_status = "manifest: fallback crate (no game mount)\n";
    std::string startup_scene;
    std::string scene_document_json;
    std::string scene_status = "scene: none\n";
    float clear_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    SceneEnvironment environment{};
    bool editor_grid_visible = true;
    TransformMode transform_mode = TransformMode::Translate;
    RenderDebugMode render_debug_mode = RenderDebugMode::Off;
    bool viewport_camera_valid = false;
    ViewportCameraState viewport_camera{};
    std::vector<ScenePlayerStart> player_starts;
    std::vector<ScenePlayerController> player_controllers;
    std::vector<ScenePrimitive> primitives;
    SceneCamera camera;
};

struct PlayModeState {
    bool active = false;
    bool collision_debug = false;
    bool input_captured = false;
    // scene::World is the authoritative scene: it holds the render prop entities
    // and the player's TransformWS. Jolt (character_spine) is the SOLVER —
    // static bodies are projected from the ECS Colliders at play-start and the
    // player capsule's solved transform is written back into the ECS pawn each
    // tick (see EcsCharacterBridge.h / ADR-019). No parallel mutable scene.
    std::unique_ptr<psynder::scene::World> sim_world;
    psynder::Entity pawn{};                          // ECS pawn (TransformWS)
    character_spine::World* phys_world = nullptr;     // Jolt static world + capsule
    character_spine::Character* phys_pawn = nullptr;  // Jolt CharacterVirtual
    // ECS prop entity -> its projected Jolt static body. When a prop is
    // destroyed (e.g. a shot crate) we remove_static_body its handle so no
    // invisible collider is left behind (the collision-ghost fix).
    std::vector<phys::EcsStaticBody> static_body_map;
    // ECS prop entity -> its Jolt dynamic body (ADR-019 class 2). Synced back
    // to the ECS each tick; shooting one applies a knockback impulse.
    std::vector<phys::EcsDynamicBody> dynamic_body_map;
    std::uint32_t dynamic_body_count = 0;
    // Deterministic input replay (#38): record one InputFrame per fixed tick
    // while playing; `replay` resets the world and drives the sim from the
    // recorded stream so the session reproduces (motion, look, and shots).
    ReplayMode replay_mode = ReplayMode::Off;
    psynder::scene::ReplayRecorder recorder;
    std::vector<psynder::scene::InputFrame> replay_frames;  // owned during playback
    psynder::scene::ReplayPlayer replay_player;
    std::uint32_t replay_tick = 0;
    bool replay_prev_fire = false;
    bool replay_fire_request = false;
    float fixed_accum_seconds = 0.0f;
    float yaw_deg = 180.0f;
    float pitch_deg = 0.0f;
    float capsule_radius_m = 0.38f;
    float capsule_height_m = 1.82f;
    float eye_height_m = 1.62f;
    psynder::math::Vec3 eye_position{0.0f, 1.62f, 0.0f};
    std::uint32_t static_collider_count = 0;
    std::string spawn_label;

    // DOTS mass-agent crowd (ADR-019 class 1): ~24 steering agents spawned at
    // play-start that chase the player and flow around the static crates/walls.
    // Agents are ordinary ECS entities (TransformWS + Agent + AgentVelocity +
    // AgentTarget + Collider + RenderMaterial) so the existing render walk +
    // raycast see them like any other prop. The scratch + StaticColliders
    // buffers persist across ticks (reused, zero per-tick heap alloc — the
    // fixed-tick determinism contract). The static-collider AABBs are gathered
    // once at play-start (the same immovable set projected to Jolt).
    psynder::physics::agents::AgentScratch agent_scratch;
    std::vector<psynder::Entity>   agent_static_entities;  // StaticColliders ids
    std::vector<psynder::math::Aabb> agent_static_aabbs;   // StaticColliders boxes
    std::vector<psynder::Entity>   crowd;                  // spawned agent entities
};

// Fracture a shot static crate into a burst of Jolt dynamic shards (ADR-021/#45).
// Delegates the deterministic spawn to the destruction lane; the seed is the
// destroyed entity's raw id, so a replay (#38) reproduces the identical break.
inline std::uint32_t fracture_static_crate(PlayModeState& play,
                                           const psynder::scene::TransformWS& xf,
                                           const psynder::scene::Collider& col,
                                           const psynder::scene::RenderMaterial& mat,
                                           const psynder::math::Vec3& shot_dir,
                                           std::uint64_t seed) {
    if (!play.sim_world || !play.phys_world) return 0;
    return psynder::physics::destruction::spawn_fracture_shards(
        play.phys_world, *play.sim_world, xf, col, mat, shot_dir, seed,
        play.dynamic_body_map);
}

// Gather the world-space AABBs of every IMMOVABLE static collider in the sim
// world into the reusable StaticColliders backing buffers, so the agent crowd
// avoids the crates + walls. This mirrors build_jolt_statics_from_ecs's filter:
// skip dynamic bodies (they move + are projected to Jolt separately) and skip
// the agents themselves. Called once at play-start (the static set is immutable
// for the session). Box/Plane use centre +/- half_extents; Sphere uses its
// radius. Determinism: pure read of the just-spawned (deterministic) scene.
inline void gather_play_static_colliders(PlayModeState& play) {
    namespace s = psynder::scene;
    play.agent_static_entities.clear();
    play.agent_static_aabbs.clear();
    if (!play.sim_world) return;
    play.sim_world->for_each_chunk_with_entities<s::TransformWS, s::Collider>(
        [&](std::size_t n, const psynder::Entity* ents, s::TransformWS* xf,
            s::Collider* col) {
            for (std::size_t i = 0; i < n; ++i) {
                if (play.sim_world->get<s::DynamicBody>(ents[i]) != nullptr) continue;
                if (play.sim_world->get<s::Agent>(ents[i]) != nullptr) continue;
                // Skip the floor: the ground plane / oversized ground slab is not
                // a horizontal obstacle — planar grounding keeps agents on it, and
                // including it would fight the ground clamp.
                if (col[i].kind == s::ShapeKind::Plane) continue;
                if (col[i].half_extents.x > 30.0f || col[i].half_extents.z > 30.0f)
                    continue;
                const float* m = xf[i].mtw.m;
                float cx = m[12];
                float cy = m[13];
                float cz = m[14];
                psynder::math::Vec3 he;
                if (col[i].kind == s::ShapeKind::Sphere) {
                    const float r = std::max(0.05f, col[i].half_extents.x);
                    he = {r, r, r};
                } else {
                    if (col[i].kind == s::ShapeKind::Plane) {
                        cy -= col[i].half_extents.y;  // top face at the surface
                    }
                    he = {std::max(0.05f, col[i].half_extents.x),
                          std::max(0.05f, col[i].half_extents.y),
                          std::max(0.05f, col[i].half_extents.z)};
                }
                play.agent_static_entities.push_back(ents[i]);
                play.agent_static_aabbs.push_back(
                    psynder::math::Aabb{{cx - he.x, cy - he.y, cz - he.z},
                                        {cx + he.x, cy + he.y, cz + he.z}});
            }
        });
}

// View over the gathered static colliders (valid until the next gather).
inline psynder::physics::agents::StaticColliders play_static_view(
    const PlayModeState& play) {
    return psynder::physics::agents::StaticColliders{
        std::span<const psynder::Entity>(play.agent_static_entities.data(),
                                         play.agent_static_entities.size()),
        std::span<const psynder::math::Aabb>(play.agent_static_aabbs.data(),
                                             play.agent_static_aabbs.size())};
}

// Spawn the steering crowd near the dynamic-crate stack (ADR-019 class 1). Fixed
// count + fixed ring positions + fixed params => the crowd spawns deterministically
// (no RNG, no wall-clock). Each agent carries Agent/AgentVelocity/AgentTarget AND
// a Collider + teal RenderMaterial, so the existing render walk draws it and the
// hitscan path can raycast it like any prop. Target is overwritten each tick.
inline void spawn_play_crowd(PlayModeState& play, const psynder::math::Vec3& spawn,
                             float spawn_yaw_rad) {
    namespace s = psynder::scene;
    if (!play.sim_world) return;
    constexpr int kCrowd = 24;
    constexpr float kRadius = 0.4f;
    constexpr float kHeight = 1.8f;  // humanoid capsule (total, incl. hemispheres)
    const float fx = std::sin(spawn_yaw_rad);
    const float fz = std::cos(spawn_yaw_rad);
    // Ring centred a few metres ahead of the player, behind the crate stack.
    const float cx = spawn.x + fx * 6.0f;
    const float cz = spawn.z + fz * 6.0f;
    play.crowd.clear();
    play.crowd.reserve(kCrowd);
    for (int i = 0; i < kCrowd; ++i) {
        const float ang = (static_cast<float>(i) / static_cast<float>(kCrowd)) *
                          6.2831853f;
        const float ring = 3.0f + static_cast<float>(i % 3) * 0.8f;
        // Centre the capsule at half its height so its feet sit on the ground.
        const psynder::math::Vec3 pos{cx + std::cos(ang) * ring, kHeight * 0.5f,
                                      cz + std::sin(ang) * ring};
        s::Agent a{};
        a.max_speed_mps = 3.0f;   // ~jog
        a.max_force = 9.0f;       // m/s^2
        a.radius_m = kRadius;     // capsule radius
        a.arrive_radius_m = 1.5f;
        a.height_m = kHeight;     // humanoid capsule => planar clamp grounds it
        const psynder::Entity e = s::spawn_agent(*play.sim_world, a, pos, spawn);
        // Render + raycast as a teal capsule (radius .x, half-height .y).
        play.sim_world->add(e, s::Collider{s::ShapeKind::Capsule,
                                           {kRadius, kHeight * 0.5f, kRadius}});
        play.sim_world->add(e, s::RenderMaterial{{0.10f, 0.70f, 0.66f}, 0.55f, 0.0f,
                                                 {0.02f, 0.18f, 0.17f}, 0.6f});
        play.crowd.push_back(e);
    }
}

// One fixed-tick drive of the crowd: point every agent at the player's CURRENT
// world position (the chase goal, taken from the already-deterministic sim) then
// advance the DOTS steering system, which writes each agent's TransformWS. Call
// once per fixed tick from the play loop, AFTER step_fixed so the goal is the
// player's post-step position. Deterministic: pure consumer of the sim state.
inline void drive_play_crowd(PlayModeState& play, const psynder::math::Vec3& goal,
                             float dt) {
    namespace s = psynder::scene;
    if (!play.sim_world || play.crowd.empty()) return;
    for (const psynder::Entity e : play.crowd) {
        if (auto* tgt = play.sim_world->get<s::AgentTarget>(e)) tgt->goal = goal;
    }
    // Ground crowd: planar steering (XZ only) + ground clamp so agents stay on the
    // floor, plus the hard static non-penetration so they flow around walls/crates.
    psynder::physics::agents::AgentTuning tune{};
    tune.planar = true;
    tune.ground_y = 0.0f;
    psynder::physics::agents::update_agents(*play.sim_world, play_static_view(play),
                                            play.agent_scratch, dt, tune);
}

struct PrimitiveVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float col[4];
};
static_assert(sizeof(PrimitiveVertex) == 48, "primitive vertex stride must stay 48 bytes");

struct PrimitiveMeshGpu {
    psynder::gpu::Handle<psynder::gpu::Buffer> vertex_buffer;
    psynder::gpu::Handle<psynder::gpu::Buffer> index_buffer;
    std::uint32_t index_count = 0;
};

struct PrimitiveRenderResources {
    psynder::shader::PipelineHandle scene_pipeline{};
    psynder::shader::PipelineHandle overlay_pipeline{};
    psynder::shader::PipelineHandle overlay_depth_pipeline{};
    // Translucent WIREFRAME outline pipeline for `debugdraw colliders`
    // (depth-test on, depth-write off, blend on, FillMode::Wireframe).
    psynder::shader::PipelineHandle wireframe_pipeline{};
    PrimitiveMeshGpu cube;
    PrimitiveMeshGpu arrow;
    PrimitiveMeshGpu sphere;
    PrimitiveMeshGpu capsule;
    PrimitiveMeshGpu plane;
    PrimitiveMeshGpu grid;
    PrimitiveMeshGpu sky;
    psynder::gpu::Handle<psynder::gpu::Texture> scene_depth;
    std::uint32_t scene_depth_width = 0;
    std::uint32_t scene_depth_height = 0;
    bool ready = false;
};

struct ScenePushConstants {
    float mvp[16];
    float material[4];
    float params[4];
    float light[4];
    float ambient_exposure[4];
};
static_assert(sizeof(ScenePushConstants) == 128, "scene push constants must fit the GPU ABI");

std::uint16_t configured_editor_ipc_port() {
    if (const char* env = std::getenv("PSYNDER_GX_EDITOR_IPC_PORT");
        env != nullptr && env[0] != '\0') {
        const int port = std::atoi(env);
        if (port > 0 && port <= 65535) {
            return static_cast<std::uint16_t>(port);
        }
    }
    return static_cast<std::uint16_t>(PSYNDER_GX_EDITOR_IPC_DEFAULT_PORT);
}

#if defined(PSYNDER_GX_PLATFORM_MACOS)
bool relaunch_current_process(int argc, char** argv, bool quiet) {
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr || argv[0][0] == '\0') {
        return false;
    }
    std::vector<char*> child_argv;
    child_argv.reserve(static_cast<std::size_t>(argc) + 1u);
    for (int i = 0; i < argc; ++i) {
        child_argv.push_back(argv[i]);
    }
    child_argv.push_back(nullptr);

    pid_t pid = 0;
    const char* exe = argv[0];
    const bool has_path = std::strchr(exe, '/') != nullptr;
    const int rc = has_path
        ? ::posix_spawn(&pid, exe, nullptr, nullptr, child_argv.data(), environ)
        : ::posix_spawnp(&pid, exe, nullptr, nullptr, child_argv.data(), environ);
    if (rc != 0) {
        if (!quiet) {
            std::fprintf(stderr,
                         "[%s] restart failed: posix_spawn rc=%d\n",
                         PSYNDER_GX_APP_LOG_PREFIX,
                         rc);
        }
        return false;
    }
    if (!quiet) {
        std::printf("[%s] restart launched pid %d\n",
                    PSYNDER_GX_APP_LOG_PREFIX,
                    static_cast<int>(pid));
    }
    return true;
}
#endif

bool parse_mount_spec(std::string_view spec, std::pair<std::string, std::string>& out) {
    const std::size_t eq = spec.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 >= spec.size()) {
        return false;
    }
    out.first = std::string{spec.substr(0, eq)};
    out.second = std::string{spec.substr(eq + 1)};
    return true;
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--smoke-frames=", 15) == 0) {
            args.smoke_frames = std::atoi(argv[i] + 15);
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            args.quiet = true;
        } else if (std::strncmp(argv[i], "--mount=", 8) == 0) {
            std::pair<std::string, std::string> mount;
            if (parse_mount_spec(std::string_view{argv[i] + 8}, mount)) {
                args.mounts.push_back(std::move(mount));
            }
        } else if (std::strcmp(argv[i], "--mount") == 0 && i + 1 < argc) {
            std::pair<std::string, std::string> mount;
            if (parse_mount_spec(std::string_view{argv[++i]}, mount)) {
                args.mounts.push_back(std::move(mount));
            }
        } else if (std::strncmp(argv[i], "--manifest=", 11) == 0) {
            args.manifest = std::string{argv[i] + 11};
        } else if (std::strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
            args.manifest = std::string{argv[++i]};
        } else if (std::strncmp(argv[i], "--cooked=", 9) == 0) {
            args.mounts.emplace_back("game", std::string{argv[i] + 9});
        } else if (std::strcmp(argv[i], "--cooked") == 0 && i + 1 < argc) {
            args.mounts.emplace_back("game", std::string{argv[++i]});
        }
    }
    return args;
}

bool ends_with(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

std::string strip_mount_prefix(std::string_view virtual_path, std::string_view mount_name) {
    const std::string colon_slash = std::string{mount_name} + ":/";
    const std::string colon = std::string{mount_name} + ":";
    if (virtual_path.substr(0, colon_slash.size()) == colon_slash) {
        return std::string{virtual_path.substr(colon_slash.size())};
    }
    if (virtual_path.substr(0, colon.size()) == colon) {
        return std::string{virtual_path.substr(colon.size())};
    }
    return std::string{virtual_path};
}

bool mount_game_vfs(const Args& args, bool quiet) {
    bool mounted_game = false;
    if (args.mounts.empty() && std::filesystem::is_directory("projects/PsyArcadeGX")) {
        const bool ok = psynder::asset::Vfs::Get().mount_directory("projects/PsyArcadeGX");
        if (ok && !quiet) {
            std::printf("[%s] mounted default project: projects/PsyArcadeGX\n",
                        PSYNDER_GX_APP_LOG_PREFIX);
        }
        mounted_game = mounted_game || ok;
    }
    for (const auto& mount : args.mounts) {
        if (mount.first != "game") {
            if (!quiet) {
                std::fprintf(stderr,
                             "[%s] ignoring unsupported mount '%s' (only game= is wired)\n",
                             PSYNDER_GX_APP_LOG_PREFIX,
                             mount.first.c_str());
            }
            continue;
        }
        const bool ok = ends_with(mount.second, ".lmpak")
            ? psynder::asset::Vfs::Get().mount_pak(mount.second)
            : psynder::asset::Vfs::Get().mount_directory(mount.second);
        mounted_game = mounted_game || ok;
        if (!ok && !quiet) {
            std::fprintf(stderr,
                         "[%s] failed to mount game VFS root: %s\n",
                         PSYNDER_GX_APP_LOG_PREFIX,
                         mount.second.c_str());
        }
    }
    return mounted_game;
}

std::string parse_json_string_value(std::string_view text, std::string_view key) {
    const std::size_t key_pos = text.find(key);
    if (key_pos == std::string_view::npos) {
        return {};
    }
    const std::size_t colon = text.find(':', key_pos + key.size());
    if (colon == std::string_view::npos) {
        return {};
    }
    std::size_t q0 = colon + 1;
    while (q0 < text.size() && std::isspace(static_cast<unsigned char>(text[q0]))) {
        ++q0;
    }
    if (q0 >= text.size() || text[q0] != '"') {
        return {};
    }
    const std::size_t q1 = q0 == std::string_view::npos
        ? std::string_view::npos
        : text.find('"', q0 + 1);
    if (q0 == std::string_view::npos || q1 == std::string_view::npos) {
        return {};
    }
    return std::string{text.substr(q0 + 1, q1 - q0 - 1)};
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
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::size_t find_json_matching(std::string_view text,
                               std::size_t open_pos,
                               char open_char,
                               char close_char) noexcept;

std::size_t find_json_scalar_end(std::string_view text, std::size_t value_begin) noexcept {
    if (value_begin >= text.size()) {
        return std::string_view::npos;
    }
    if (text[value_begin] == '"') {
        bool escaped = false;
        for (std::size_t i = value_begin + 1; i < text.size(); ++i) {
            const char c = text[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                return i + 1;
            }
        }
        return std::string_view::npos;
    }
    std::size_t end = value_begin;
    while (end < text.size() && text[end] != ',' && text[end] != '}' &&
           text[end] != '\n' && text[end] != '\r') {
        ++end;
    }
    return end;
}

std::size_t find_json_value_end(std::string_view text, std::size_t value_begin) noexcept {
    if (value_begin >= text.size()) {
        return std::string_view::npos;
    }
    if (text[value_begin] == '{') {
        const std::size_t end = find_json_matching(text, value_begin, '{', '}');
        return end == std::string_view::npos ? end : end + 1u;
    }
    if (text[value_begin] == '[') {
        const std::size_t end = find_json_matching(text, value_begin, '[', ']');
        return end == std::string_view::npos ? end : end + 1u;
    }
    return find_json_scalar_end(text, value_begin);
}

std::string selected_entity_value_json(std::string_view entity_name) {
    if (entity_name.empty()) {
        return "null";
    }
    return "\"" + json_escape(entity_name) + "\"";
}

bool patch_selected_entity_json(std::string& document_json,
                                std::string_view entity_name) {
    const std::string value = selected_entity_value_json(entity_name);
    constexpr std::string_view kKey = "\"selected_entity\"";
    const std::size_t key_pos = document_json.find(kKey);
    if (key_pos != std::string::npos) {
        const std::size_t colon = document_json.find(':', key_pos + kKey.size());
        if (colon == std::string::npos) {
            return false;
        }
        std::size_t value_begin = colon + 1;
        while (value_begin < document_json.size() &&
               std::isspace(static_cast<unsigned char>(document_json[value_begin]))) {
            ++value_begin;
        }
        const std::size_t value_end = find_json_scalar_end(document_json, value_begin);
        if (value_end == std::string_view::npos || value_end < value_begin) {
            return false;
        }
        document_json.replace(value_begin, value_end - value_begin, value);
        return true;
    }

    const std::size_t path_pos = document_json.find("\"path\"");
    if (path_pos != std::string::npos) {
        const std::size_t line_end = document_json.find('\n', path_pos);
        if (line_end != std::string::npos) {
            document_json.insert(line_end + 1,
                                 "  \"selected_entity\": " + value + ",\n");
            return true;
        }
    }

    const std::size_t root_open = document_json.find('{');
    if (root_open == std::string::npos) {
        return false;
    }
    document_json.insert(root_open + 1,
                         "\n  \"selected_entity\": " + value + ",");
    return true;
}

std::size_t find_json_matching(std::string_view text,
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

std::size_t find_json_field_value_begin(std::string_view text,
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
    std::size_t value_begin = colon + 1;
    while (value_begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[value_begin]))) {
        ++value_begin;
    }
    return value_begin;
}

std::string parse_json_string_field(std::string_view text,
                                    std::string_view key,
                                    std::size_t search_from = 0) {
    const std::size_t value_begin =
        find_json_field_value_begin(text, key, search_from);
    if (value_begin == std::string_view::npos ||
        value_begin >= text.size() ||
        text[value_begin] != '"') {
        return {};
    }
    bool escaped = false;
    std::string out;
    for (std::size_t i = value_begin + 1; i < text.size(); ++i) {
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

bool parse_json_float_field(std::string_view text,
                            std::string_view key,
                            float& out) {
    const std::size_t value_begin = find_json_field_value_begin(text, key);
    if (value_begin == std::string_view::npos) {
        return false;
    }
    std::string scratch{text.substr(value_begin, std::min<std::size_t>(64u, text.size() - value_begin))};
    char* end = nullptr;
    const float parsed = std::strtof(scratch.c_str(), &end);
    if (end == scratch.c_str()) {
        return false;
    }
    out = parsed;
    return true;
}

bool parse_json_vec3_field(std::string_view text,
                           std::string_view key,
                           psynder::math::Vec3& out) {
    const std::size_t value_begin = find_json_field_value_begin(text, key);
    if (value_begin == std::string_view::npos ||
        value_begin >= text.size() ||
        text[value_begin] != '[') {
        return false;
    }
    const std::size_t value_end =
        find_json_matching(text, value_begin, '[', ']');
    if (value_end == std::string_view::npos || value_end <= value_begin) {
        return false;
    }
    std::string scratch{text.substr(value_begin + 1u, value_end - value_begin - 1u)};
    const char* cursor = scratch.c_str();
    char* end = nullptr;
    float values[3]{};
    for (float& value : values) {
        while (*cursor != '\0' &&
               (std::isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',')) {
            ++cursor;
        }
        value = std::strtof(cursor, &end);
        if (end == cursor) {
            return false;
        }
        cursor = end;
    }
    out = {values[0], values[1], values[2]};
    return true;
}

bool find_json_object_field(std::string_view text,
                            std::string_view key,
                            std::size_t& out_begin,
                            std::size_t& out_end) noexcept {
    const std::size_t value_begin = find_json_field_value_begin(text, key);
    if (value_begin == std::string_view::npos ||
        value_begin >= text.size() ||
        text[value_begin] != '{') {
        return false;
    }
    const std::size_t close = find_json_matching(text, value_begin, '{', '}');
    if (close == std::string_view::npos) {
        return false;
    }
    out_begin = value_begin;
    out_end = close + 1u;
    return true;
}

std::string transform_mode_token(TransformMode mode) noexcept {
    switch (mode) {
        case TransformMode::Rotate: return "rotate";
        case TransformMode::Scale: return "scale";
        case TransformMode::Translate: break;
    }
    return "translate";
}

TransformMode transform_mode_from_token(std::string_view token) noexcept {
    if (token == "rotate" || token == "r") {
        return TransformMode::Rotate;
    }
    if (token == "scale" || token == "s") {
        return TransformMode::Scale;
    }
    return TransformMode::Translate;
}

std::string render_debug_token(RenderDebugMode mode) noexcept {
    switch (mode) {
        case RenderDebugMode::Depth: return "depth";
        case RenderDebugMode::Off: break;
    }
    return "off";
}

RenderDebugMode render_debug_from_token(std::string_view token) noexcept {
    if (token == "depth" || token == "z" || token == "1" || token == "on") {
        return RenderDebugMode::Depth;
    }
    return RenderDebugMode::Off;
}

bool find_scene_entity_object(std::string_view document_json,
                              std::string_view entity_name,
                              std::size_t& out_begin,
                              std::size_t& out_end) noexcept {
    const std::size_t entities_value =
        find_json_field_value_begin(document_json, "entities");
    if (entities_value == std::string_view::npos ||
        entities_value >= document_json.size() ||
        document_json[entities_value] != '[') {
        return false;
    }
    const std::size_t entities_end =
        find_json_matching(document_json, entities_value, '[', ']');
    if (entities_end == std::string_view::npos) {
        return false;
    }
    std::size_t cursor = entities_value + 1;
    while (cursor < entities_end) {
        const std::size_t object_begin = document_json.find('{', cursor);
        if (object_begin == std::string_view::npos || object_begin >= entities_end) {
            break;
        }
        const std::size_t object_end =
            find_json_matching(document_json, object_begin, '{', '}');
        if (object_end == std::string_view::npos || object_end > entities_end) {
            break;
        }
        const std::string_view object =
            document_json.substr(object_begin, object_end - object_begin + 1);
        if (parse_json_string_field(object, "name") == entity_name) {
            out_begin = object_begin;
            out_end = object_end + 1;
            return true;
        }
        cursor = object_end + 1;
    }
    return false;
}

std::string vec3_json(psynder::math::Vec3 value) {
    char buf[128]{};
    std::snprintf(buf,
                  sizeof(buf),
                  "[%.6g, %.6g, %.6g]",
                  static_cast<double>(value.x),
                  static_cast<double>(value.y),
                  static_cast<double>(value.z));
    return std::string{buf};
}

std::string viewport_camera_json(const EditorCameraState& camera) {
    char buf[320]{};
    std::snprintf(buf,
                  sizeof(buf),
                  "{\n"
                  "      \"position\": %s,\n"
                  "      \"rotation_euler_deg\": %s,\n"
                  "      \"fov_y_deg\": %.6g\n"
                  "    }",
                  vec3_json(camera.position).c_str(),
                  vec3_json(camera.rotation_euler_deg).c_str(),
                  static_cast<double>(camera.fov_y_deg));
    return std::string{buf};
}

bool patch_editor_setting_json(std::string& document_json,
                               std::string_view key,
                               std::string_view raw_value_json) {
    std::size_t editor_begin = 0;
    std::size_t editor_end = 0;
    if (!find_json_object_field(document_json, "editorSettings", editor_begin, editor_end)) {
        const std::size_t root_open = document_json.find('{');
        if (root_open == std::string::npos) {
            return false;
        }
        const std::string setting =
            "\n  \"editorSettings\": {\n"
            "    \"" + std::string{key} + "\": " + std::string{raw_value_json} + "\n"
            "  },";
        document_json.insert(root_open + 1u, setting);
        return true;
    }

    const std::string needle = "\"" + std::string{key} + "\"";
    const std::string_view editor =
        std::string_view{document_json}.substr(editor_begin, editor_end - editor_begin);
    const std::size_t local_key = editor.find(needle);
    if (local_key != std::string_view::npos) {
        const std::size_t local_value_begin =
            find_json_field_value_begin(editor, key, local_key);
        if (local_value_begin == std::string_view::npos) {
            return false;
        }
        const std::size_t value_begin = editor_begin + local_value_begin;
        const std::size_t value_end =
            find_json_value_end(document_json, value_begin);
        if (value_end == std::string_view::npos || value_end < value_begin) {
            return false;
        }
        document_json.replace(value_begin,
                              value_end - value_begin,
                              raw_value_json.data(),
                              raw_value_json.size());
        return true;
    }

    const std::size_t insert_at = editor_begin + 1u;
    const bool has_existing_fields =
        editor.find(':') != std::string_view::npos;
    const std::string field =
        "\n    \"" + std::string{key} + "\": " + std::string{raw_value_json} +
        (has_existing_fields ? "," : "") ;
    document_json.insert(insert_at, field);
    return true;
}

bool patch_entity_vec3_json(std::string& document_json,
                            std::string_view entity_name,
                            std::string_view field_name,
                            psynder::math::Vec3 value_vec) {
    std::size_t object_begin = 0;
    std::size_t object_end = 0;
    if (!find_scene_entity_object(document_json, entity_name, object_begin, object_end)) {
        return false;
    }
    const std::string_view object =
        std::string_view{document_json}.substr(object_begin, object_end - object_begin);
    const std::size_t local_value_begin =
        find_json_field_value_begin(object, field_name);
    const std::string value = vec3_json(value_vec);
    if (local_value_begin == std::string_view::npos) {
        const std::size_t insert_at = object_end > 0 ? object_end - 1 : object_end;
        document_json.insert(insert_at,
                             ",\n      \"" + std::string{field_name} + "\": " + value);
        return true;
    }
    const std::size_t value_begin = object_begin + local_value_begin;
    if (value_begin >= document_json.size() || document_json[value_begin] != '[') {
        return false;
    }
    const std::size_t value_end =
        find_json_matching(document_json, value_begin, '[', ']');
    if (value_end == std::string_view::npos) {
        return false;
    }
    document_json.replace(value_begin, value_end - value_begin + 1, value);
    return true;
}

bool patch_entity_position_json(std::string& document_json,
                                std::string_view entity_name,
                                psynder::math::Vec3 position) {
    return patch_entity_vec3_json(document_json, entity_name, "position", position);
}

struct EditorSnapSettings {
    bool enabled = false;
    float step = 1.0f;
};

EditorSnapSettings parse_editor_snap_settings(std::string_view document_json) {
    EditorSnapSettings out{};
    const std::string mode = parse_json_string_field(document_json, "snap_mode");
    out.enabled = mode == "grid";
    float step = out.step;
    if (parse_json_float_field(document_json, "snap_step", step)) {
        out.step = std::clamp(step, 0.001f, 100.0f);
    }
    return out;
}

TransformMode parse_editor_transform_mode(std::string_view document_json) {
    return transform_mode_from_token(parse_json_string_field(document_json, "transform_mode"));
}

RenderDebugMode parse_editor_render_debug_mode(std::string_view document_json) {
    return render_debug_from_token(parse_json_string_field(document_json, "render_debug"));
}

bool parse_editor_viewport_camera(std::string_view document_json,
                                  ViewportCameraState& out) {
    std::size_t editor_begin = 0;
    std::size_t editor_end = 0;
    if (!find_json_object_field(document_json, "editorSettings", editor_begin, editor_end)) {
        return false;
    }
    const std::string_view editor =
        document_json.substr(editor_begin, editor_end - editor_begin);
    std::size_t camera_begin = 0;
    std::size_t camera_end = 0;
    if (!find_json_object_field(editor, "viewport_camera", camera_begin, camera_end)) {
        return false;
    }
    const std::string_view camera =
        editor.substr(camera_begin, camera_end - camera_begin);
    ViewportCameraState parsed = out;
    (void)parse_json_vec3_field(camera, "position", parsed.position);
    (void)parse_json_vec3_field(camera, "rotation_euler_deg", parsed.rotation_euler_deg);
    (void)parse_json_float_field(camera, "fov_y_deg", parsed.fov_y_deg);
    parsed.fov_y_deg = std::clamp(parsed.fov_y_deg, 1.0f, 179.0f);
    out = parsed;
    return true;
}

bool entity_has_component(std::string_view entity_object, std::string_view component) noexcept {
    const std::size_t components_value =
        find_json_field_value_begin(entity_object, "components");
    if (components_value == std::string_view::npos ||
        components_value >= entity_object.size() ||
        entity_object[components_value] != '[') {
        return false;
    }
    const std::size_t components_end =
        find_json_matching(entity_object, components_value, '[', ']');
    if (components_end == std::string_view::npos) {
        return false;
    }
    const std::string needle = "\"" + std::string{component} + "\"";
    const std::string_view list =
        entity_object.substr(components_value, components_end - components_value + 1);
    return list.find(needle) != std::string_view::npos;
}

bool parse_player_start(std::string_view document_json,
                        psynder::math::Vec3& out_position,
                        psynder::math::Vec3& out_rotation_deg,
                        std::string& out_name) {
    const std::size_t entities_value =
        find_json_field_value_begin(document_json, "entities");
    if (entities_value == std::string_view::npos ||
        entities_value >= document_json.size() ||
        document_json[entities_value] != '[') {
        return false;
    }
    const std::size_t entities_end =
        find_json_matching(document_json, entities_value, '[', ']');
    if (entities_end == std::string_view::npos) {
        return false;
    }

    std::size_t cursor = entities_value + 1;
    while (cursor < entities_end) {
        const std::size_t object_begin = document_json.find('{', cursor);
        if (object_begin == std::string_view::npos || object_begin >= entities_end) {
            break;
        }
        const std::size_t object_end =
            find_json_matching(document_json, object_begin, '{', '}');
        if (object_end == std::string_view::npos || object_end > entities_end) {
            break;
        }
        const std::string_view object =
            document_json.substr(object_begin, object_end - object_begin + 1);
        if (entity_has_component(object, "PlayerStart")) {
            out_name = parse_json_string_field(object, "name");
            (void)parse_json_vec3_field(object, "position", out_position);
            (void)parse_json_vec3_field(object, "rotation_euler_deg", out_rotation_deg);
            return true;
        }
        cursor = object_end + 1;
    }
    return false;
}

psynder::math::Vec3 play_spawn_position(const PlayerBootState& boot,
                                        const EditorCameraState& editor_camera,
                                        std::string& out_label,
                                        float& out_yaw_deg,
                                        float& out_pitch_deg) {
    if (!boot.player_starts.empty()) {
        const ScenePlayerStart& start = boot.player_starts.front();
        out_label = start.name.empty() ? "PlayerStart" : start.name;
        out_yaw_deg = start.rotation_euler_deg.y;
        out_pitch_deg = start.rotation_euler_deg.x;
        return start.position;
    }
    if (!boot.player_controllers.empty()) {
        const ScenePlayerController& controller = boot.player_controllers.front();
        out_label = controller.name.empty() ? "PlayerController" : controller.name;
        out_yaw_deg = controller.rotation_euler_deg.y;
        out_pitch_deg = controller.rotation_euler_deg.x;
        return controller.position;
    }
    psynder::math::Vec3 position{};
    psynder::math::Vec3 rotation{};
    if (parse_player_start(boot.scene_document_json, position, rotation, out_label)) {
        out_yaw_deg = rotation.y;
        out_pitch_deg = rotation.x;
        return position;
    }
    out_label = boot.camera.active ? boot.camera.name : "editor camera";
    const psynder::math::Vec3 camera_pos = editor_camera.initialized
        ? editor_camera.position
        : boot.camera.position;
    const psynder::math::Vec3 camera_rot = editor_camera.initialized
        ? editor_camera.rotation_euler_deg
        : boot.camera.rotation_euler_deg;
    out_yaw_deg = camera_rot.y;
    out_pitch_deg = camera_rot.x;
    return {camera_pos.x, std::max(0.05f, camera_pos.y - 1.62f), camera_pos.z};
}

psynder::math::Quat primitive_rotation_quat(const ScenePrimitive& primitive) noexcept;

struct PlayModeTuning {
    float walk_speed_mps = 4.6f;
    float run_speed_mps = 7.0f;
    float mouse_sensitivity_deg_per_px = 0.10f;
    float capsule_radius_m = 0.38f;
    float capsule_height_m = 1.82f;
    float eye_height_m = 1.62f;
};

PlayModeTuning play_tuning_from_controller(const ScenePlayerController& controller) {
    PlayModeTuning tuning{};
    tuning.walk_speed_mps = controller.walk_speed;
    tuning.run_speed_mps = controller.run_speed;
    tuning.mouse_sensitivity_deg_per_px = controller.mouse_sensitivity;
    tuning.capsule_radius_m = controller.capsule_radius;
    tuning.capsule_height_m = controller.capsule_height;
    tuning.eye_height_m = std::min(1.62f, std::max(0.1f, controller.capsule_height - 0.13f));
    return tuning;
}

PlayModeTuning authored_play_tuning(const PlayerBootState& boot) {
    if (!boot.player_controllers.empty()) {
        return play_tuning_from_controller(boot.player_controllers.front());
    }
    return {};
}

float console_cvar_float_override(std::string_view name, float fallback) {
    if (auto* cvar = psynder::console::Console::Get().FindCVar(name); cvar != nullptr) {
        if (cvar->value == cvar->default_value) {
            return fallback;
        }
        return cvar->GetFloat();
    }
    return fallback;
}

PlayModeTuning current_play_tuning(const PlayerBootState& boot) {
    PlayModeTuning tuning = authored_play_tuning(boot);
    tuning.walk_speed_mps =
        std::clamp(console_cvar_float_override("play_walk_speed", tuning.walk_speed_mps),
                   0.1f,
                   20.0f);
    tuning.run_speed_mps =
        std::clamp(console_cvar_float_override("play_run_speed", tuning.run_speed_mps),
                   tuning.walk_speed_mps,
                   30.0f);
    tuning.mouse_sensitivity_deg_per_px =
        std::clamp(console_cvar_float_override("play_mouse_sensitivity",
                                               tuning.mouse_sensitivity_deg_per_px),
                   0.005f,
                   2.0f);
    tuning.capsule_radius_m =
        std::clamp(console_cvar_float_override("play_capsule_radius", tuning.capsule_radius_m),
                   0.15f,
                   1.25f);
    tuning.capsule_height_m =
        std::clamp(console_cvar_float_override("play_capsule_height", tuning.capsule_height_m),
                   tuning.capsule_radius_m * 2.2f,
                   3.0f);
    tuning.eye_height_m =
        std::clamp(console_cvar_float_override("play_eye_height", tuning.eye_height_m),
                   tuning.capsule_radius_m,
                   tuning.capsule_height_m);
    return tuning;
}

bool token_is_enabled(std::string_view token) noexcept {
    return token == "1" || token == "on" || token == "true" || token == "yes";
}

bool console_cvar_bool(std::string_view name, bool fallback) {
    if (auto* cvar = psynder::console::Console::Get().FindCVar(name); cvar != nullptr) {
        return cvar->GetBool();
    }
    return fallback;
}

void stop_play_mode(PlayModeState& play) {
    const bool keep_collision_debug = play.collision_debug;
    if (play.phys_world) {
        character_spine::destroy_world(play.phys_world);  // also frees the pawn
        play.phys_world = nullptr;
        play.phys_pawn = nullptr;
    }
    play = {};  // resets sim_world (destroys the ECS sim world) + all fields
    play.collision_debug = keep_collision_debug;
}

void print_play_state(const PlayModeState& play, psynder::console::Output& out) {
    out.FormatLine("play_state: {}", play.active ? "playing" : "editing");
    out.FormatLine("input_capture: {}", play.input_captured ? "captured" : "released");
    out.FormatLine("spawn: {}", play.spawn_label.empty() ? "<none>" : play.spawn_label);
    out.FormatLine("eye: {:.2f} {:.2f} {:.2f}",
                   play.eye_position.x,
                   play.eye_position.y,
                   play.eye_position.z);
    out.FormatLine("yaw_pitch: {:.1f} {:.1f}", play.yaw_deg, play.pitch_deg);
    out.FormatLine("capsule: r {:.2f} h {:.2f}",
                   play.capsule_radius_m,
                   play.capsule_height_m);
    out.FormatLine("static_colliders: {}", play.static_collider_count);
    out.FormatLine("dynamic_bodies: {}", play.dynamic_body_count);
    const char* replay_label =
        play.replay_mode == ReplayMode::Recording ? "recording"
        : play.replay_mode == ReplayMode::Replaying ? "replaying"
        : "off";
    out.FormatLine("replay: {} ({} recorded ticks)", replay_label, play.recorder.size());
    out.FormatLine("collision_debug: {}", play.collision_debug ? "on" : "off");
}

bool start_play_mode(PlayModeState& play,
                     const PlayerBootState& boot,
                     const EditorCameraState& editor_camera,
                     psynder::console::Output* out = nullptr) {
    if (play.active) {
        if (out) out->PrintLine("play: already active");
        return true;
    }
    if (boot.render_state != PlayerBootRenderState::SceneRendering) {
        if (out) out->PrintLine("play: needs a loaded scene with an active camera");
        return false;
    }

    auto world = std::make_unique<psynder::scene::World>();

    // Import the authored scene primitives into the ECS as canonical prop
    // entities (TransformWS + Collider + RenderMaterial) — these are the same
    // entities the play renderer draws and the character controller collides
    // against. No parallel character_spine collider set.
    bool has_static_ground = false;
    std::uint32_t static_collider_count = 0;
    for (const ScenePrimitive& primitive : boot.primitives) {
        if (!primitive.visible) continue;
        if (primitive.kind == ScenePrimitiveKind::Plane) {
            has_static_ground = true;
        }
        psynder::scene::spawn_prop(*world,
                                   psynder::sample02::prop_from_primitive(primitive));
        ++static_collider_count;
    }
    if (!has_static_ground) {
        // Fallback ground slab: top face at y == 0.
        psynder::scene::PropDesc ground;
        ground.position = {0.0f, -0.5f, 0.0f};
        ground.shape = psynder::scene::ShapeKind::Box;
        ground.half_extents = {128.0f, 0.5f, 128.0f};
        ground.material.albedo = {0.32f, 0.34f, 0.38f};
        psynder::scene::spawn_prop(*world, ground);
        ++static_collider_count;
    }

    const PlayModeTuning tuning = current_play_tuning(boot);
    float yaw = 180.0f;
    float pitch = 0.0f;
    std::string spawn_label;
    const psynder::math::Vec3 spawn =
        play_spawn_position(boot, editor_camera, spawn_label, yaw, pitch);
    phys::CharacterDesc pawn_desc{};
    pawn_desc.foot_position = spawn;
    pawn_desc.radius_m = tuning.capsule_radius_m;
    pawn_desc.height_m = tuning.capsule_height_m;
    const psynder::Entity pawn = phys::spawn_character(*world, pawn_desc);

    // Spawn a small stack of dynamic crates in front of the spawn so play mode
    // immediately shows Jolt rigid bodies (ADR-019 class 2): they fall, stack,
    // and get knocked when shot. They carry scene::DynamicBody, so the static
    // projection skips them and build_jolt_dynamics_from_ecs picks them up.
    {
        const float spawn_yaw_rad = yaw * psynder::math::kDegToRad;
        const float fx = std::sin(spawn_yaw_rad);
        const float fz = std::cos(spawn_yaw_rad);
        const float bx = spawn.x + fx * 3.0f;
        const float bz = spawn.z + fz * 3.0f;
        for (int level = 0; level < 3; ++level) {
            psynder::scene::DynamicPropDesc crate;
            crate.position = {bx, 0.5f + static_cast<float>(level) * 1.02f, bz};
            crate.half_extents = {0.5f, 0.5f, 0.5f};
            crate.mass_kg = 8.0f;
            crate.material.albedo = {0.86f, 0.46f, 0.18f};  // orange = dynamic
            psynder::scene::spawn_dynamic_prop(*world, crate);
        }
    }

    // Build the Jolt solver world: project the ECS static colliders into Jolt
    // (immutable for the session) and create the player capsule. The ECS pawn's
    // TransformWS is written from the solved capsule each tick — Jolt is the
    // solver, the ECS stays authoritative (ADR-019 / EcsCharacterBridge.h).
    character_spine::WorldDesc world_desc{};
    // Headroom for fracture (ADR-021/#45): a shot static crate is replaced by a
    // 2x2x2 = 8-shard burst of dynamic bodies. Budget for every static collider
    // plus its potential shards so a play session can fracture freely.
    world_desc.max_bodies = static_cast<std::uint32_t>(
        std::max<std::size_t>(512,
                              static_cast<std::size_t>(static_collider_count) * 9 + 64));
    character_spine::World* phys_world = character_spine::create_world(world_desc);
    if (!phys_world) {
        if (out) out->PrintLine("play: failed to create Jolt world");
        return false;
    }
    std::vector<phys::EcsStaticBody> static_body_map =
        phys::build_jolt_statics_from_ecs(phys_world, *world);
    std::vector<phys::EcsDynamicBody> dynamic_body_map =
        phys::build_jolt_dynamics_from_ecs(phys_world, *world);
    character_spine::CapsuleDesc cap_desc{};
    cap_desc.foot_position_m[0] = spawn.x;
    cap_desc.foot_position_m[1] = spawn.y;
    cap_desc.foot_position_m[2] = spawn.z;
    cap_desc.radius_m = tuning.capsule_radius_m;
    cap_desc.height_m = tuning.capsule_height_m;
    cap_desc.max_horizontal_speed_mps = tuning.run_speed_mps;
    character_spine::Character* phys_pawn =
        character_spine::create_capsule_character(phys_world, cap_desc);
    if (!phys_pawn) {
        character_spine::destroy_world(phys_world);
        if (out) out->PrintLine("play: failed to create pawn capsule");
        return false;
    }

    play.active = true;
    play.input_captured = console_cvar_bool("play_capture_input", true);
    play.sim_world = std::move(world);
    play.pawn = pawn;
    play.phys_world = phys_world;
    play.phys_pawn = phys_pawn;
    play.static_body_map = std::move(static_body_map);
    play.dynamic_body_count = static_cast<std::uint32_t>(dynamic_body_map.size());
    play.dynamic_body_map = std::move(dynamic_body_map);
    play.fixed_accum_seconds = 0.0f;
    play.yaw_deg = yaw;
    play.pitch_deg = std::clamp(pitch, -85.0f, 85.0f);
    play.capsule_radius_m = tuning.capsule_radius_m;
    play.capsule_height_m = tuning.capsule_height_m;
    play.eye_height_m = tuning.eye_height_m;
    play.eye_position = {spawn.x, spawn.y + play.eye_height_m, spawn.z};
    play.static_collider_count = static_collider_count;
    play.spawn_label = std::move(spawn_label);
    // Spawn the DOTS steering crowd (ADR-019 class 1) AFTER the Jolt static
    // projection so agents are never projected as immovable bodies — they are
    // movers solved by the custom agent system, not Jolt. Gather the immutable
    // static-collider AABBs once so the crowd flows around the crates/walls.
    spawn_play_crowd(play, spawn, yaw * psynder::math::kDegToRad);
    gather_play_static_colliders(play);
    // Record this session's per-tick input so it can be replayed deterministically.
    play.replay_mode = ReplayMode::Recording;
    play.recorder.clear();
    play.recorder.reserve(120u * 60u);  // ~60 s headroom at 120 Hz
    play.replay_tick = 0;
    if (out) {
        out->FormatLine("play: started from {} ({} static colliders)",
                        play.spawn_label,
                        play.static_collider_count);
    }
    return true;
}

// Reset the play world to its start state and replay the recorded input stream
// (#38). Because the player capsule, Jolt step, and dynamic crates are all
// deterministic, feeding the same per-tick inputs into a fresh world reproduces
// the session — motion, look, and shots.
bool start_replay(PlayModeState& play,
                  const PlayerBootState& boot,
                  const EditorCameraState& editor_camera,
                  psynder::console::Output* out = nullptr) {
    if (play.recorder.empty()) {
        if (out) out->PrintLine("replay: nothing recorded yet (play and move first)");
        return false;
    }
    // Snapshot the recorded stream before stop_play_mode() wipes the recorder.
    std::vector<psynder::scene::InputFrame> frames(
        play.recorder.frames().begin(), play.recorder.frames().end());
    stop_play_mode(play);
    if (!start_play_mode(play, boot, editor_camera, out)) {
        return false;
    }
    play.replay_frames = std::move(frames);
    play.replay_player = psynder::scene::ReplayPlayer(
        std::span<const psynder::scene::InputFrame>(play.replay_frames.data(),
                                                    play.replay_frames.size()));
    play.replay_mode = ReplayMode::Replaying;
    play.replay_tick = 0;
    play.fixed_accum_seconds = 0.0f;
    play.replay_prev_fire = false;
    play.replay_fire_request = false;
    if (out) {
        out->FormatLine("replay: playing back {} recorded ticks",
                        play.replay_frames.size());
    }
    return true;
}

bool update_play_mode(PlayModeState& play,
                      const PlayerBootState& boot,
                      const psynder::platform::Input* input,
                      const psynder::platform::MouseState& mouse,
                      float dt_seconds) {
    namespace m = psynder::math;
    namespace p = psynder::platform;
    if (!play.active || !play.sim_world || !play.phys_world || !play.phys_pawn ||
        !input || boot.render_state != PlayerBootRenderState::SceneRendering) {
        return false;
    }

    constexpr float kFixedDt = 1.0f / 120.0f;
    const PlayModeTuning tuning = current_play_tuning(boot);

    // ----- REPLAY: drive the sim from the recorded input stream, not live input.
    // The world was reset to play-start by start_replay(), so feeding the same
    // per-tick stream into the same deterministic Jolt step reproduces the run.
    if (play.replay_mode == ReplayMode::Replaying) {
        namespace rs = psynder::scene;
        play.fixed_accum_seconds = std::min(play.fixed_accum_seconds + dt_seconds, 0.25f);
        while (play.fixed_accum_seconds >= kFixedDt && !play.replay_player.done()) {
            const rs::InputFrame& f = play.replay_player.next();
            play.yaw_deg = f.yaw_deg;
            play.pitch_deg = f.pitch_deg;
            const float rspeed = (f.buttons & rs::kReplayBtnRun)
                ? tuning.run_speed_mps : tuning.walk_speed_mps;
            character_spine::CharacterInput rcin{};
            rcin.velocity_mps[0] = f.move_x * rspeed;
            rcin.velocity_mps[1] = f.move_z * rspeed;
            rcin.jump = (f.buttons & rs::kReplayBtnJump) != 0u;
            rcin.crouch = (f.buttons & rs::kReplayBtnCrouch) != 0u;
            character_spine::set_character_input(play.phys_pawn, rcin);
            if (auto* in = play.sim_world->get<phys::CharacterInput>(play.pawn)) {
                in->move_dir_x = f.move_x;
                in->move_dir_z = f.move_z;
                in->speed_mps = rspeed;
                in->jump = rcin.jump ? 1u : 0u;
            }
            const bool fire_now = (f.buttons & rs::kReplayBtnFire) != 0u;
            if (fire_now && !play.replay_prev_fire) play.replay_fire_request = true;
            play.replay_prev_fire = fire_now;
            character_spine::step_fixed(play.phys_world);
            // Chase one fixed tick: target = player's post-step foot position.
            {
                const character_spine::Transform pt =
                    character_spine::character_transform(play.phys_pawn);
                drive_play_crowd(play,
                                 {pt.foot_position_m[0], pt.foot_position_m[1],
                                  pt.foot_position_m[2]},
                                 kFixedDt);
            }
            play.fixed_accum_seconds -= kFixedDt;
            ++play.replay_tick;
        }
        if (play.replay_player.done()) {
            play.replay_mode = ReplayMode::Off;  // playback finished — freeze the pawn
        }
        phys::sync_dynamics_to_ecs(play.phys_world, *play.sim_world, play.dynamic_body_map);
        const character_spine::Transform rt = character_spine::character_transform(play.phys_pawn);
        const m::Vec3 rfoot{rt.foot_position_m[0], rt.foot_position_m[1], rt.foot_position_m[2]};
        if (auto* xf = play.sim_world->get<psynder::scene::TransformWS>(play.pawn)) {
            xf->prev_mtw = xf->mtw;
            xf->mtw = phys::foot_transform(rfoot);
        }
        play.eye_position = {rfoot.x, rfoot.y + play.eye_height_m, rfoot.z};
        return true;
    }

    const bool shift_down =
        input->key_down(p::KeyCode::LeftShift) || input->key_down(p::KeyCode::RightShift);
    if (mouse.dx != 0.0f || mouse.dy != 0.0f) {
        play.yaw_deg -= mouse.dx * tuning.mouse_sensitivity_deg_per_px;
        play.pitch_deg =
            std::clamp(play.pitch_deg + mouse.dy * tuning.mouse_sensitivity_deg_per_px,
                       -85.0f,
                       85.0f);
    }

    const float yaw = play.yaw_deg * m::kDegToRad;
    const m::Vec3 forward = m::normalize({std::sin(yaw), 0.0f, std::cos(yaw)});
    // Camera screen-right is cross(forward, up) (see math::look_at_rh): for
    // up=+Y that is (-cos(yaw), 0, sin(yaw)). The previous vector was its
    // negation, which made D strafe left / A strafe right.
    const m::Vec3 right = m::normalize({-std::cos(yaw), 0.0f, std::sin(yaw)});
    m::Vec3 move{0.0f, 0.0f, 0.0f};
    if (input->key_down(p::KeyCode::W)) move = m::add(move, forward);
    if (input->key_down(p::KeyCode::S)) move = m::sub(move, forward);
    if (input->key_down(p::KeyCode::D)) move = m::add(move, right);
    if (input->key_down(p::KeyCode::A)) move = m::sub(move, right);
    if (m::dot(move, move) > 0.0001f) {
        move = m::normalize(move);
    }

    const float speed = shift_down ? tuning.run_speed_mps : tuning.walk_speed_mps;

    // Feed desired motion into the Jolt CharacterVirtual and mirror it into the
    // ECS CharacterInput (gameplay-readable). Jolt solves capsule-vs-world.
    character_spine::CharacterInput cin{};
    cin.velocity_mps[0] = move.x * speed;
    cin.velocity_mps[1] = move.z * speed;
    cin.jump = input->key_pressed(p::KeyCode::Space);
    cin.crouch =
        input->key_down(p::KeyCode::LeftCtrl) || input->key_down(p::KeyCode::RightCtrl);
    character_spine::set_character_input(play.phys_pawn, cin);
    if (auto* in = play.sim_world->get<phys::CharacterInput>(play.pawn)) {
        in->move_dir_x = move.x;
        in->move_dir_z = move.z;
        in->speed_mps = speed;
        in->jump = cin.jump ? 1u : 0u;
    }

    // Per-tick input record (world-space move dir + look angles + button mask),
    // captured once per frame and emitted for each fixed tick this frame runs.
    std::uint32_t buttons = 0u;
    if (cin.jump) buttons |= psynder::scene::kReplayBtnJump;
    if (shift_down) buttons |= psynder::scene::kReplayBtnRun;
    if (cin.crouch) buttons |= psynder::scene::kReplayBtnCrouch;
    if (mouse.left) buttons |= psynder::scene::kReplayBtnFire;

    play.fixed_accum_seconds =
        std::min(play.fixed_accum_seconds + dt_seconds, 0.25f);
    while (play.fixed_accum_seconds >= kFixedDt) {
        if (play.replay_mode == ReplayMode::Recording) {
            play.recorder.record(psynder::scene::InputFrame{
                play.replay_tick, move.x, move.z, play.yaw_deg, play.pitch_deg, buttons});
            ++play.replay_tick;
        }
        character_spine::step_fixed(play.phys_world);
        // Chase one fixed tick: target = player's post-step foot position.
        {
            const character_spine::Transform pt =
                character_spine::character_transform(play.phys_pawn);
            drive_play_crowd(play,
                             {pt.foot_position_m[0], pt.foot_position_m[1],
                              pt.foot_position_m[2]},
                             kFixedDt);
        }
        play.fixed_accum_seconds -= kFixedDt;
    }

    // Write the Jolt-solved dynamic-body transforms back into the ECS so the
    // render walk + raycast see them move (ADR-019 / EcsCharacterBridge.h).
    phys::sync_dynamics_to_ecs(play.phys_world, *play.sim_world, play.dynamic_body_map);

    // Write the solved capsule transform back into the ECS pawn (authoritative)
    // and derive the eye position for the camera.
    const character_spine::Transform t = character_spine::character_transform(play.phys_pawn);
    const m::Vec3 foot{t.foot_position_m[0], t.foot_position_m[1], t.foot_position_m[2]};
    if (auto* xf = play.sim_world->get<psynder::scene::TransformWS>(play.pawn)) {
        xf->prev_mtw = xf->mtw;
        xf->mtw = phys::foot_transform(foot);
    }
    play.eye_position = {foot.x, foot.y + play.eye_height_m, foot.z};
    return true;
}

SceneCamera play_mode_camera(const PlayModeState& play, const SceneCamera& authored) {
    SceneCamera out = authored;
    out.active = true;
    out.position = play.eye_position;
    out.rotation_euler_deg = {play.pitch_deg, play.yaw_deg, 0.0f};
    return out;
}

void apply_parsed_scene_document(PlayerBootState& boot,
                                 psynder::sample02::ParsedSceneDocument parsed) {
    std::copy(std::begin(parsed.clear_rgba),
              std::end(parsed.clear_rgba),
    std::begin(boot.clear_rgba));
    boot.environment = std::move(parsed.environment);
    boot.editor_grid_visible = parsed.editor_grid_visible;
    boot.camera = std::move(parsed.camera);
    boot.player_starts = std::move(parsed.player_starts);
    boot.player_controllers = std::move(parsed.player_controllers);
    boot.primitives = std::move(parsed.primitives);
    boot.transform_mode = parse_editor_transform_mode(boot.scene_document_json);
    boot.render_debug_mode = parse_editor_render_debug_mode(boot.scene_document_json);
    boot.viewport_camera_valid =
        parse_editor_viewport_camera(boot.scene_document_json, boot.viewport_camera);
}

void apply_scene_document_to_boot(PlayerBootState& boot,
                                  std::string_view scene_path,
                                  std::string_view scene_text) {
    boot.startup_scene = std::string{scene_path};
    boot.scene_document_json = std::string{scene_text};
    char manifest_buf[256]{};
    std::snprintf(manifest_buf,
                  sizeof(manifest_buf),
                  "manifest: editor live scene\n"
                  "startup_scene: %s\n",
                  boot.startup_scene.empty() ? "none" : boot.startup_scene.c_str());
    boot.manifest_status = std::string{manifest_buf};

    psynder::sample02::ParsedSceneDocument parsed =
        psynder::sample02::parse_loose_scene_document(scene_text);
    const bool has_camera = parsed.has_active_camera;
    boot.render_state = has_camera
        ? PlayerBootRenderState::SceneRendering
        : PlayerBootRenderState::NoCameraRendering;
    apply_parsed_scene_document(boot, std::move(parsed));
    boot.scene_status = has_camera
        ? "scene: editor live\ncamera: active\n"
        : "scene: editor live\ncamera: none\n";
}

void pack_msgpack_uint(std::vector<psynder::u8>& out, psynder::u64 value) {
    if (value <= 0x7f) {
        out.push_back(static_cast<psynder::u8>(value));
    } else if (value <= 0xff) {
        out.push_back(0xcc);
        out.push_back(static_cast<psynder::u8>(value));
    } else if (value <= 0xffff) {
        out.push_back(0xcd);
        out.push_back(static_cast<psynder::u8>(value >> 8));
        out.push_back(static_cast<psynder::u8>(value));
    } else if (value <= 0xffffffffull) {
        out.push_back(0xce);
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<psynder::u8>(value >> shift));
        }
    } else {
        out.push_back(0xcf);
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<psynder::u8>(value >> shift));
        }
    }
}

void pack_msgpack_bool(std::vector<psynder::u8>& out, bool value) {
    out.push_back(value ? 0xc3 : 0xc2);
}

void pack_msgpack_array(std::vector<psynder::u8>& out, psynder::usize count) {
    if (count <= 15) {
        out.push_back(static_cast<psynder::u8>(0x90 | count));
    } else {
        out.push_back(0xdc);
        out.push_back(static_cast<psynder::u8>(count >> 8));
        out.push_back(static_cast<psynder::u8>(count));
    }
}

void pack_msgpack_string(std::vector<psynder::u8>& out, std::string_view text) {
    const psynder::usize n = text.size();
    if (n <= 31) {
        out.push_back(static_cast<psynder::u8>(0xa0 | n));
    } else if (n <= 0xff) {
        out.push_back(0xd9);
        out.push_back(static_cast<psynder::u8>(n));
    } else if (n <= 0xffff) {
        out.push_back(0xda);
        out.push_back(static_cast<psynder::u8>(n >> 8));
        out.push_back(static_cast<psynder::u8>(n));
    } else {
        out.push_back(0xdb);
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<psynder::u8>(n >> shift));
        }
    }
    out.insert(out.end(),
               reinterpret_cast<const psynder::u8*>(text.data()),
               reinterpret_cast<const psynder::u8*>(text.data()) + text.size());
}

std::string scene_status_json_for_boot(const PlayerBootState& boot,
                                       std::string_view message) {
    const std::string name =
        parse_json_string_value(boot.scene_document_json, "\"name\"");
    const std::string path =
        parse_json_string_value(boot.scene_document_json, "\"path\"");
    const bool has_scene = !boot.scene_document_json.empty();
    const bool has_camera = boot.camera.active;
    const std::size_t entity_count = boot.primitives.size() +
        boot.player_starts.size() +
        boot.player_controllers.size() +
        (has_camera ? 1u : 0u);
    std::string out;
    out.reserve(boot.scene_document_json.size() + 256);
    out += "{\"name\":\"";
    out += json_escape(name.empty() ? "Startup" : name);
    out += "\",\"path\":\"";
    out += json_escape(path.empty() ? boot.startup_scene : path);
    out += "\",\"has_scene\":";
    out += has_scene ? "true" : "false";
    out += ",\"has_camera\":";
    out += has_camera ? "true" : "false";
    out += ",\"dirty\":true,\"entity_count\":";
    out += std::to_string(entity_count);
    out += ",\"message\":\"";
    out += json_escape(message);
    out += "\",\"document_json\":\"";
    out += json_escape(boot.scene_document_json);
    out += "\"}";
    return out;
}

void broadcast_scene_sync_reply(const PlayerBootState& boot, std::string_view message) {
    constexpr psynder::u64 kSceneCommandReply = 41;
    std::vector<psynder::u8> frame;
    frame.reserve(boot.scene_document_json.size() + 256);
    pack_msgpack_uint(frame, kSceneCommandReply);
    pack_msgpack_array(frame, 5);
    pack_msgpack_uint(frame, 0);
    pack_msgpack_bool(frame, true);
    pack_msgpack_string(frame, message);
    pack_msgpack_string(frame, boot.startup_scene);
    const std::string status = scene_status_json_for_boot(boot, message);
    pack_msgpack_string(frame, status);
    psynder::editor::ipc::Server::Get().broadcast(
        "scene",
        std::span<const psynder::u8>{frame.data(), frame.size()});
}

std::string current_selected_entity(const PlayerBootState& boot) {
    const std::string document_selection =
        parse_json_string_value(boot.scene_document_json, "\"selected_entity\"");
    if (!document_selection.empty()) {
        return document_selection;
    }
    for (const ScenePrimitive& primitive : boot.primitives) {
        if (primitive.selected) {
            return primitive.name;
        }
    }
    for (const ScenePlayerStart& player_start : boot.player_starts) {
        if (player_start.selected) {
            return player_start.name;
        }
    }
    for (const ScenePlayerController& controller : boot.player_controllers) {
        if (controller.selected) {
            return controller.name;
        }
    }
    return {};
}

bool sync_editor_selection(PlayerBootState& boot, std::string_view selected_entity) {
    if (boot.scene_document_json.empty() ||
        current_selected_entity(boot) == selected_entity) {
        return false;
    }
    std::string next_document = boot.scene_document_json;
    if (!patch_selected_entity_json(next_document, selected_entity)) {
        return false;
    }
    apply_scene_document_to_boot(boot, boot.startup_scene, next_document);
    psynder::editor::ipc::seed_scene_document(boot.startup_scene,
                                              "Startup",
                                              boot.scene_document_json);
    broadcast_scene_sync_reply(boot, "synced scene");
    return true;
}

bool sync_editor_entity_position(PlayerBootState& boot,
                                 std::string_view entity_name,
                                 psynder::math::Vec3 position,
                                 std::string_view message = "synced scene") {
    if (boot.scene_document_json.empty() || entity_name.empty()) {
        return false;
    }
    std::string next_document = boot.scene_document_json;
    if (!patch_entity_position_json(next_document, entity_name, position)) {
        return false;
    }
    apply_scene_document_to_boot(boot, boot.startup_scene, next_document);
    psynder::editor::ipc::seed_scene_document(boot.startup_scene,
                                              "Startup",
                                              boot.scene_document_json);
    broadcast_scene_sync_reply(boot, message);
    return true;
}

bool sync_editor_entity_vec3(PlayerBootState& boot,
                             std::string_view entity_name,
                             std::string_view field_name,
                             psynder::math::Vec3 value,
                             std::string_view message) {
    if (boot.scene_document_json.empty() || entity_name.empty()) {
        return false;
    }
    std::string next_document = boot.scene_document_json;
    if (!patch_entity_vec3_json(next_document, entity_name, field_name, value)) {
        return false;
    }
    apply_scene_document_to_boot(boot, boot.startup_scene, next_document);
    psynder::editor::ipc::seed_scene_document(boot.startup_scene,
                                              "Startup",
                                              boot.scene_document_json);
    broadcast_scene_sync_reply(boot, message);
    return true;
}

bool sync_editor_transform_mode(PlayerBootState& boot,
                                TransformMode mode,
                                std::string_view message = "tool mode update") {
    if (boot.scene_document_json.empty()) {
        boot.transform_mode = mode;
        return false;
    }
    std::string next_document = boot.scene_document_json;
    const std::string raw = "\"" + transform_mode_token(mode) + "\"";
    if (!patch_editor_setting_json(next_document, "transform_mode", raw)) {
        return false;
    }
    apply_scene_document_to_boot(boot, boot.startup_scene, next_document);
    psynder::editor::ipc::seed_scene_document(boot.startup_scene,
                                              "Startup",
                                              boot.scene_document_json);
    broadcast_scene_sync_reply(boot, message);
    return true;
}

bool sync_editor_render_debug(PlayerBootState& boot,
                              RenderDebugMode mode,
                              std::string_view message = "render debug update") {
    if (boot.scene_document_json.empty()) {
        boot.render_debug_mode = mode;
        return false;
    }
    std::string next_document = boot.scene_document_json;
    const std::string raw = "\"" + render_debug_token(mode) + "\"";
    if (!patch_editor_setting_json(next_document, "render_debug", raw)) {
        return false;
    }
    apply_scene_document_to_boot(boot, boot.startup_scene, next_document);
    psynder::editor::ipc::seed_scene_document(boot.startup_scene,
                                              "Startup",
                                              boot.scene_document_json);
    broadcast_scene_sync_reply(boot, message);
    return true;
}

bool sync_editor_viewport_camera(PlayerBootState& boot,
                                 const EditorCameraState& editor_camera,
                                 std::string_view message = "viewport camera update") {
    if (boot.scene_document_json.empty() || !editor_camera.initialized) {
        return false;
    }
    std::string next_document = boot.scene_document_json;
    const std::string raw = viewport_camera_json(editor_camera);
    if (!patch_editor_setting_json(next_document, "viewport_camera", raw)) {
        return false;
    }
    apply_scene_document_to_boot(boot, boot.startup_scene, next_document);
    psynder::editor::ipc::seed_scene_document(boot.startup_scene,
                                              "Startup",
                                              boot.scene_document_json);
    broadcast_scene_sync_reply(boot, message);
    return true;
}

bool scene_entity_exists(const PlayerBootState& boot, std::string_view entity_name) noexcept {
    if (entity_name.empty()) {
        return false;
    }
    if (boot.camera.active && entity_name == std::string_view{boot.camera.name}) {
        return true;
    }
    for (const ScenePlayerStart& player_start : boot.player_starts) {
        if (!player_start.name.empty() && entity_name == std::string_view{player_start.name}) {
            return true;
        }
    }
    for (const ScenePlayerController& controller : boot.player_controllers) {
        if (!controller.name.empty() && entity_name == std::string_view{controller.name}) {
            return true;
        }
    }
    for (const ScenePrimitive& primitive : boot.primitives) {
        if (!primitive.name.empty() && entity_name == std::string_view{primitive.name}) {
            return true;
        }
    }
    return false;
}

std::string selected_entity_runtime_label(const PlayerBootState& boot) {
    const std::string selected = current_selected_entity(boot);
    if (selected.empty()) {
        return "<none>";
    }
    if (boot.camera.active && selected == boot.camera.name) {
        return selected + " (camera)";
    }
    for (const ScenePlayerStart& player_start : boot.player_starts) {
        if (selected == player_start.name) {
            return selected + " (player start)";
        }
    }
    for (const ScenePlayerController& controller : boot.player_controllers) {
        if (selected == controller.name) {
            return selected + " (player controller)";
        }
    }
    for (const ScenePrimitive& primitive : boot.primitives) {
        if (selected != primitive.name) {
            continue;
        }
        const char* kind = "cube";
        if (primitive.kind == ScenePrimitiveKind::Sphere) {
            kind = "sphere";
        } else if (primitive.kind == ScenePrimitiveKind::Plane) {
            kind = "plane";
        }
        return selected + " (" + kind + ")";
    }
    return selected + " (missing)";
}

void reconcile_runtime_selection(PlayerBootState& boot) {
    const std::string selected = current_selected_entity(boot);
    if (!selected.empty() && !scene_entity_exists(boot, selected)) {
        (void)sync_editor_selection(boot, {});
    }
}

void register_sample_editor_commands(PlayerBootState& boot,
                                     PlayModeState* play,
                                     EditorCameraState* editor_camera) {
    auto& con = psynder::console::Console::Get();
    if (con.FindCVar("play_walk_speed") == nullptr) {
        con.RegisterCVar("play_walk_speed", "4.6", "FPS play-mode walk speed in metres/second.");
    }
    if (con.FindCVar("play_run_speed") == nullptr) {
        con.RegisterCVar("play_run_speed", "7.0", "FPS play-mode run speed in metres/second.");
    }
    if (con.FindCVar("play_mouse_sensitivity") == nullptr) {
        con.RegisterCVar("play_mouse_sensitivity",
                         "0.10",
                         "FPS play-mode look sensitivity in degrees per mouse pixel.");
    }
    if (con.FindCVar("play_capsule_radius") == nullptr) {
        con.RegisterCVar("play_capsule_radius", "0.38", "FPS pawn capsule radius in metres.");
    }
    if (con.FindCVar("play_capsule_height") == nullptr) {
        con.RegisterCVar("play_capsule_height", "1.82", "FPS pawn capsule height in metres.");
    }
    if (con.FindCVar("play_eye_height") == nullptr) {
        con.RegisterCVar("play_eye_height", "1.62", "FPS camera eye height above pawn foot.");
    }
    if (con.FindCVar("play_capture_input") == nullptr) {
        con.RegisterCVar("play_capture_input",
                         "1",
                         "Capture raw relative mouse input while FPS play mode is active.");
    }
    if (con.FindCommand("tool_mode") == nullptr) {
        con.RegisterCommand(
            "tool_mode",
            "Set editor transform mode: translate, rotate, or scale.",
            [&boot](std::span<const std::string_view> args,
                    psynder::console::Output& out) {
                if (args.empty()) {
                    out.FormatLine("tool_mode: {}", transform_mode_token(boot.transform_mode));
                    return;
                }
                const TransformMode mode = transform_mode_from_token(args[0]);
                (void)sync_editor_transform_mode(boot, mode);
                out.FormatLine("tool_mode: {}", transform_mode_token(boot.transform_mode));
            });
    }
    const auto register_mode_alias = [&con, &boot](const char* name,
                                                   TransformMode mode,
                                                   const char* description) {
        if (con.FindCommand(name) != nullptr) {
            return;
        }
        con.RegisterCommand(
            name,
            description,
            [&boot, mode](std::span<const std::string_view>,
                          psynder::console::Output& out) {
                (void)sync_editor_transform_mode(boot, mode);
                out.FormatLine("tool_mode: {}", transform_mode_token(boot.transform_mode));
            });
    };
    register_mode_alias("translate", TransformMode::Translate, "Switch editor gizmo to translate.");
    register_mode_alias("rotate", TransformMode::Rotate, "Switch editor gizmo to rotate.");
    register_mode_alias("scale", TransformMode::Scale, "Switch editor gizmo to scale.");

    if (con.FindCommand("render_debug") == nullptr) {
        con.RegisterCommand(
            "render_debug",
            "Set scene render debug mode: off or depth.",
            [&boot](std::span<const std::string_view> args,
                    psynder::console::Output& out) {
                if (args.empty()) {
                    out.FormatLine("render_debug: {}", render_debug_token(boot.render_debug_mode));
                    return;
                }
                const RenderDebugMode mode = render_debug_from_token(args[0]);
                (void)sync_editor_render_debug(boot, mode);
                out.FormatLine("render_debug: {}", render_debug_token(boot.render_debug_mode));
            });
    }
    if (con.FindCommand("debugdraw") == nullptr) {
        con.RegisterCommand(
            "debugdraw",
            "Toggle play-mode debug draw: 'colliders' (collision volumes), "
            "'flow' (agent heading + target), or 'off'.",
            [](std::span<const std::string_view> args,
               psynder::console::Output& out) {
                if (!args.empty()) {
                    const std::string_view a = args[0];
                    if (a == "colliders") {
                        g_debug_draw_colliders = !g_debug_draw_colliders;
                    } else if (a == "flow") {
                        g_debug_draw_flow = !g_debug_draw_flow;
                    } else if (a == "off") {
                        g_debug_draw_colliders = false;
                        g_debug_draw_flow = false;
                    } else {
                        out.FormatLine(
                            "debugdraw: unknown '{}' (use colliders|flow|off)", a);
                        return;
                    }
                }
                out.FormatLine("debugdraw: colliders={} flow={}",
                               g_debug_draw_colliders, g_debug_draw_flow);
            });
    }
    if (con.FindCommand("depth_view") == nullptr) {
        con.RegisterCommand(
            "depth_view",
            "Toggle distance-coded scene depth debug view.",
            [&boot](std::span<const std::string_view> args,
                    psynder::console::Output& out) {
                const bool enable = args.empty()
                    ? boot.render_debug_mode != RenderDebugMode::Depth
                    : (render_debug_from_token(args[0]) == RenderDebugMode::Depth);
                (void)sync_editor_render_debug(
                    boot,
                    enable ? RenderDebugMode::Depth : RenderDebugMode::Off);
                out.FormatLine("render_debug: {}", render_debug_token(boot.render_debug_mode));
            });
    }
    if (play && editor_camera && con.FindCommand("play") == nullptr) {
        con.RegisterCommand(
            "play",
            "Start FPS play mode from PlayerStart or the editor camera.",
            [&boot, play, editor_camera](std::span<const std::string_view>,
                                         psynder::console::Output& out) {
                (void)start_play_mode(*play, boot, *editor_camera, &out);
            });
    }
    if (play && con.FindCommand("stop") == nullptr) {
        con.RegisterCommand(
            "stop",
            "Stop FPS play mode and return to editor camera control.",
            [play](std::span<const std::string_view>,
                   psynder::console::Output& out) {
                if (!play->active) {
                    out.PrintLine("stop: play mode is not active");
                    return;
                }
                stop_play_mode(*play);
                out.PrintLine("stop: returned to editor mode");
            });
    }
    if (play && editor_camera && con.FindCommand("play_toggle") == nullptr) {
        con.RegisterCommand(
            "play_toggle",
            "Toggle FPS play mode.",
            [&boot, play, editor_camera](std::span<const std::string_view>,
                                         psynder::console::Output& out) {
                if (play->active) {
                    stop_play_mode(*play);
                    out.PrintLine("play_toggle: stopped");
                    return;
                }
                (void)start_play_mode(*play, boot, *editor_camera, &out);
            });
    }
    if (play && editor_camera && con.FindCommand("replay") == nullptr) {
        con.RegisterCommand(
            "replay",
            "Reset play mode and replay the recorded input stream (#38).",
            [&boot, play, editor_camera](std::span<const std::string_view>,
                                         psynder::console::Output& out) {
                (void)start_replay(*play, boot, *editor_camera, &out);
            });
    }
    if (play && con.FindCommand("play_state") == nullptr) {
        con.RegisterCommand(
            "play_state",
            "Print FPS play-mode spawn, tuning, and collision debug state.",
            [play](std::span<const std::string_view>,
                   psynder::console::Output& out) {
                print_play_state(*play, out);
            });
    }
    if (play && con.FindCommand("input_capture") == nullptr) {
        con.RegisterCommand(
            "input_capture",
            "Set FPS play-mode input capture: on or off.",
            [play](std::span<const std::string_view> args,
                   psynder::console::Output& out) {
                if (args.empty()) {
                    play->input_captured = !play->input_captured;
                } else {
                    play->input_captured = token_is_enabled(args[0]);
                }
                out.FormatLine("input_capture: {}",
                               play->input_captured ? "captured" : "released");
            });
    }
    if (play && con.FindCommand("collision_debug") == nullptr) {
        con.RegisterCommand(
            "collision_debug",
            "Toggle FPS pawn collision marker overlay: on or off.",
            [play](std::span<const std::string_view> args,
                   psynder::console::Output& out) {
                if (args.empty()) {
                    play->collision_debug = !play->collision_debug;
                } else {
                    play->collision_debug = token_is_enabled(args[0]);
                }
                out.FormatLine("collision_debug: {}", play->collision_debug ? "on" : "off");
            });
    }
}

struct ManifestReadState {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string text;
};

void manifest_loaded(psynder::asset::Blob blob, void* user) noexcept {
    auto* state = static_cast<ManifestReadState*>(user);
    if (blob.data && blob.bytes > 0) {
        state->text.assign(reinterpret_cast<const char*>(blob.data), blob.bytes);
        state->ok = true;
    }
    state->done.store(true, std::memory_order_release);
}

std::string read_vfs_text_async(std::string_view virtual_path, bool& ok) {
    ManifestReadState state;
    psynder::asset::Vfs::Get().read_async(virtual_path, &manifest_loaded, &state);
    while (!state.done.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    ok = state.ok;
    return std::move(state.text);
}

std::string manifest_field(std::string_view text, std::string_view key) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t eol = text.find('\n', pos);
        std::string_view line = text.substr(
            pos,
            eol == std::string_view::npos ? text.size() - pos : eol - pos);
        pos = eol == std::string_view::npos ? text.size() : eol + 1;

        const std::size_t hash = line.find('#');
        if (hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        const std::size_t found = line.find(key);
        if (found == std::string_view::npos) {
            continue;
        }
        line.remove_prefix(found + key.size());
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        line.remove_prefix(eq + 1);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
            line.remove_prefix(1);
        }
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.remove_suffix(1);
        }
        if (line.size() >= 2 && line.front() == '"' && line.back() == '"') {
            line.remove_prefix(1);
            line.remove_suffix(1);
        }
        return std::string{line};
    }
    return {};
}

PlayerBootState boot_player_project(const Args& args, bool quiet) {
    PlayerBootState boot{};
    const bool mounted_game = mount_game_vfs(args, quiet);
    if (!mounted_game) {
        boot.manifest_status = "manifest: missing (no game mount)\n";
        boot.scene_status = "scene: no scene loaded\n";
        return boot;
    }

    bool manifest_ok = false;
    const std::string manifest_path = strip_mount_prefix(args.manifest, "game");
    const std::string manifest_text = read_vfs_text_async(manifest_path, manifest_ok);

    if (!manifest_ok) {
        if (!quiet) {
            std::fprintf(stderr,
                         "[%s] game manifest missing: %s\n",
                         PSYNDER_GX_APP_LOG_PREFIX,
                         args.manifest.c_str());
        }
        boot.manifest_status = "manifest: missing\n";
        boot.scene_status = "scene: no scene loaded\n";
        return boot;
    }

    boot.startup_scene = manifest_field(manifest_text, "startup_scene");
    if (!quiet) {
        std::printf("[%s] loaded %s (%zu bytes)\n",
                    PSYNDER_GX_APP_LOG_PREFIX,
                    args.manifest.c_str(),
                    manifest_text.size());
    }
    char buf[192]{};
    std::snprintf(buf,
                  sizeof(buf),
                  "manifest: loaded (%zu bytes)\n"
                  "startup_scene: %s\n",
                  manifest_text.size(),
                  boot.startup_scene.empty() ? "none" : boot.startup_scene.c_str());
    boot.manifest_status = std::string{buf};

    if (boot.startup_scene.empty()) {
        boot.scene_status = "scene: no scene loaded\n";
        return boot;
    }

    bool scene_ok = false;
    const std::string scene_path = strip_mount_prefix(boot.startup_scene, "game");
    const std::string scene_text = read_vfs_text_async(scene_path, scene_ok);
    if (!scene_ok) {
        if (!quiet) {
            std::fprintf(stderr,
                         "[%s] startup scene missing: %s\n",
                         PSYNDER_GX_APP_LOG_PREFIX,
                         boot.startup_scene.c_str());
        }
        boot.scene_status = "scene: no scene loaded\n";
        return boot;
    }

    psynder::sample02::ParsedSceneDocument parsed =
        psynder::sample02::parse_loose_scene_document(scene_text);
    const bool has_camera = parsed.has_active_camera;
    boot.scene_document_json = scene_text;
    boot.render_state = has_camera
        ? PlayerBootRenderState::SceneRendering
        : PlayerBootRenderState::NoCameraRendering;
    apply_parsed_scene_document(boot, std::move(parsed));
    boot.scene_status = has_camera
        ? "scene: loaded\ncamera: active\n"
        : "scene: loaded\ncamera: none\n";
    return boot;
}

std::string gpu_info_text(psynder::gpu::Device* dev, const char* backend) {
    char buf[384]{};
    std::snprintf(buf,
                  sizeof(buf),
                  "backend: %s\n"
                  "device: %s\n"
                  "unified_memory: %s\n"
                  "raytracing: %s\n"
                  "mesh_shaders: %s\n",
                  backend,
                  psynder::gpu::device_name(dev),
                  psynder::gpu::device_is_unified_memory(dev) ? "yes" : "no",
                  psynder::gpu::device_supports_rt(dev) ? "yes" : "no",
                  psynder::gpu::device_supports_mesh_shaders(dev) ? "yes" : "no");
    return std::string{buf};
}

psynder::scene::TransformWS crate_transform(float t_sec) noexcept {
    namespace m = psynder::math;
    psynder::scene::TransformWS out{};
    const m::Mat4 spin = m::rotate_quat(
        m::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, t_sec * 0.35f));
    const m::Mat4 move = m::translate({0.0f, 0.5f, 0.0f});
    out.mtw = m::mul(move, spin);
    out.prev_mtw = out.mtw;
    return out;
}

psynder::camera::CameraState default_camera() noexcept {
    psynder::camera::CameraState cam{};
    cam.position[0] = 0.0f;
    cam.position[1] = 1.2f;
    cam.position[2] = 4.0f;
    cam.yaw_deg = 0.0f;
    cam.pitch_deg = -8.0f;
    cam.fov_y_deg = 60.0f;
    cam.near_m = 0.05f;
    cam.far_m = 200.0f;
    return cam;
}

psynder::math::Vec3 camera_forward(const SceneCamera& camera) noexcept {
    namespace m = psynder::math;
    const float pitch = camera.rotation_euler_deg.x * m::kDegToRad;
    const float yaw = camera.rotation_euler_deg.y * m::kDegToRad;
    return m::normalize({
        std::cos(pitch) * std::sin(yaw),
        -std::sin(pitch),
        std::cos(pitch) * std::cos(yaw),
    });
}

psynder::math::Vec3 camera_right(const SceneCamera& camera) noexcept {
    namespace m = psynder::math;
    return m::normalize(m::cross(camera_forward(camera), {0.0f, 1.0f, 0.0f}));
}

SceneCamera editor_camera_to_scene_camera(const EditorCameraState& editor_camera,
                                          const SceneCamera& authored_camera) {
    SceneCamera out = authored_camera;
    out.position = editor_camera.position;
    out.rotation_euler_deg = editor_camera.rotation_euler_deg;
    out.fov_y_deg = editor_camera.fov_y_deg;
    out.active = authored_camera.active;
    return out;
}

void sync_editor_camera_to_authored(EditorCameraState& editor_camera,
                                    const SceneCamera& authored_camera,
                                    bool force) {
    if (!authored_camera.active) {
        return;
    }
    const bool camera_changed =
        editor_camera.source_camera_name != authored_camera.name;
    if (!force && editor_camera.initialized &&
        editor_camera.user_override && !camera_changed) {
        return;
    }
    editor_camera.position = authored_camera.position;
    editor_camera.rotation_euler_deg = authored_camera.rotation_euler_deg;
    editor_camera.fov_y_deg = authored_camera.fov_y_deg;
    editor_camera.initialized = true;
    editor_camera.user_override = false;
    editor_camera.orbiting_prev = false;
    editor_camera.source_camera_name = authored_camera.name;
}

void sync_editor_camera_to_boot(EditorCameraState& editor_camera,
                                const PlayerBootState& boot,
                                bool force) {
    if (boot.viewport_camera_valid) {
        const std::string source = "$viewport:" + boot.camera.name;
        const bool camera_changed = editor_camera.source_camera_name != source;
        if (!force && editor_camera.initialized &&
            editor_camera.user_override && !camera_changed) {
            return;
        }
        editor_camera.position = boot.viewport_camera.position;
        editor_camera.rotation_euler_deg = boot.viewport_camera.rotation_euler_deg;
        editor_camera.fov_y_deg = boot.viewport_camera.fov_y_deg;
        editor_camera.initialized = true;
        editor_camera.user_override = false;
        editor_camera.orbiting_prev = false;
        editor_camera.source_camera_name = source;
        return;
    }
    sync_editor_camera_to_authored(editor_camera, boot.camera, force);
}

psynder::math::Vec3 editor_focus_point(const PlayerBootState& boot) noexcept {
    for (const ScenePrimitive& primitive : boot.primitives) {
        if (primitive.selected) {
            return primitive.position;
        }
    }
    if (!boot.primitives.empty()) {
        return boot.primitives.front().position;
    }
    return {0.0f, 0.5f, 0.0f};
}

ViewportCameraTuning editor_camera_tuning(const PlayerBootState& boot,
                                          const SceneCamera& camera) noexcept {
    namespace m = psynder::math;
    ViewportCameraTuning tuning{};
    tuning.focus_distance = 5.5f;
    tuning.wheel_distance = 5.5f;
    tuning.has_focus = false;
    const std::string selected = current_selected_entity(boot);
    for (const ScenePrimitive& primitive : boot.primitives) {
        if ((selected.empty() && primitive.selected) ||
            (!selected.empty() && selected == primitive.name)) {
            const float radius = std::max(
                0.75f,
                std::max(std::fabs(primitive.scale.x),
                         std::max(std::fabs(primitive.scale.y),
                                  std::fabs(primitive.scale.z))));
            tuning.focus_distance = std::clamp(radius * 4.25f, 2.25f, 24.0f);
            tuning.wheel_distance =
                std::max(tuning.focus_distance,
                         m::length(m::sub(camera.position, primitive.position)));
            tuning.has_focus = true;
            return tuning;
        }
    }
    const m::Vec3 focus = editor_focus_point(boot);
    tuning.wheel_distance = std::max(2.0f, m::length(m::sub(camera.position, focus)));
    return tuning;
}

bool update_editor_camera(EditorCameraState& editor_camera,
                          const PlayerBootState& boot,
                          const psynder::platform::Input* input,
                          const psynder::platform::MouseState& mouse,
                          float dt_seconds) {
    namespace m = psynder::math;
    namespace p = psynder::platform;
    if (!input || boot.render_state != PlayerBootRenderState::SceneRendering ||
        !boot.camera.active) {
        return false;
    }
    sync_editor_camera_to_boot(editor_camera, boot, false);
    if (!editor_camera.initialized) {
        return false;
    }

    SceneCamera camera = editor_camera_to_scene_camera(editor_camera, boot.camera);
    const bool shift_down =
        input->key_down(p::KeyCode::LeftShift) || input->key_down(p::KeyCode::RightShift);
    const bool alt_down =
        input->key_down(p::KeyCode::LeftAlt) || input->key_down(p::KeyCode::RightAlt);
    const float move_speed = shift_down ? 18.0f : 5.0f;
    const float look_sensitivity = 0.12f;
    const float pan_sensitivity = 0.0045f;

    bool changed = false;
    if (mouse.right) {
        if (mouse.dx != 0.0f || mouse.dy != 0.0f) {
            camera.rotation_euler_deg.y -= mouse.dx * look_sensitivity;
            camera.rotation_euler_deg.x =
                std::clamp(camera.rotation_euler_deg.x + mouse.dy * look_sensitivity,
                           -89.0f,
                           89.0f);
            changed = true;
        }
    }

    m::Vec3 move{0.0f, 0.0f, 0.0f};
    const m::Vec3 forward = camera_forward(camera);
    const m::Vec3 right = camera_right(camera);
    const m::Vec3 up = m::normalize(m::cross(right, forward));
    if (mouse.right) {
        if (input->key_down(p::KeyCode::W)) move = m::add(move, forward);
        if (input->key_down(p::KeyCode::S)) move = m::sub(move, forward);
        if (input->key_down(p::KeyCode::D)) move = m::add(move, right);
        if (input->key_down(p::KeyCode::A)) move = m::sub(move, right);
        if (input->key_down(p::KeyCode::E)) move = m::add(move, up);
        if (input->key_down(p::KeyCode::Q)) move = m::sub(move, up);
    }
    if (mouse.right && m::dot(move, move) > 0.0001f) {
        camera.position = m::add(camera.position,
                                 m::mul(m::normalize(move), move_speed * dt_seconds));
        changed = true;
    }

    if (mouse.middle && (mouse.dx != 0.0f || mouse.dy != 0.0f)) {
        const float distance_scale =
            std::max(1.0f, m::length(m::sub(camera.position, editor_focus_point(boot))));
        camera.position = m::sub(camera.position,
                                 m::mul(right, mouse.dx * pan_sensitivity * distance_scale));
        camera.position = m::add(camera.position,
                                 m::mul(up, mouse.dy * pan_sensitivity * distance_scale));
        changed = true;
    }

    const bool orbiting = alt_down && mouse.left;
    if (orbiting && (mouse.dx != 0.0f || mouse.dy != 0.0f || !editor_camera.orbiting_prev)) {
        const m::Vec3 target = editor_focus_point(boot);
        const float distance =
            std::max(0.25f, m::length(m::sub(camera.position, target)));
        camera.rotation_euler_deg.y += mouse.dx * look_sensitivity;
        camera.rotation_euler_deg.x =
            std::clamp(camera.rotation_euler_deg.x + mouse.dy * look_sensitivity,
                       -89.0f,
                       89.0f);
        const m::Vec3 orbit_forward = camera_forward(camera);
        camera.position = m::sub(target, m::mul(orbit_forward, distance));
        changed = true;
    }
    editor_camera.orbiting_prev = orbiting;

    if (mouse.wheel != 0.0f) {
        const ViewportCameraTuning tuning = editor_camera_tuning(boot, camera);
        const float wheel_delta = std::clamp(mouse.wheel, -2.0f, 2.0f);
        const float wheel_speed =
            std::clamp(tuning.wheel_distance * (shift_down ? 0.055f : 0.024f),
                       0.035f,
                       shift_down ? 1.35f : 0.65f);
        camera.position = m::add(camera.position,
                                 m::mul(camera_forward(camera), wheel_delta * wheel_speed));
        changed = true;
    }

    if (input->key_pressed(p::KeyCode::F)) {
        const m::Vec3 target = editor_focus_point(boot);
        const ViewportCameraTuning tuning = editor_camera_tuning(boot, camera);
        const float distance =
            tuning.has_focus
                ? tuning.focus_distance
                : std::max(3.0f, m::length(m::sub(camera.position, target)));
        camera.position = m::sub(target, m::mul(camera_forward(camera), distance));
        changed = true;
    }

    if (changed) {
        editor_camera.position = camera.position;
        editor_camera.rotation_euler_deg = camera.rotation_euler_deg;
        editor_camera.fov_y_deg = camera.fov_y_deg;
        editor_camera.user_override = true;
    }
    return changed;
}

psynder::math::Quat primitive_rotation_quat(const ScenePrimitive& primitive) noexcept {
    namespace m = psynder::math;
    const m::Quat rot_x = m::quat_from_axis_angle(
        {1.0f, 0.0f, 0.0f},
        primitive.rotation_euler_deg.x * m::kDegToRad);
    const m::Quat rot_y = m::quat_from_axis_angle(
        {0.0f, 1.0f, 0.0f},
        primitive.rotation_euler_deg.y * m::kDegToRad);
    const m::Quat rot_z = m::quat_from_axis_angle(
        {0.0f, 0.0f, 1.0f},
        primitive.rotation_euler_deg.z * m::kDegToRad);
    return m::quat_normalize(m::quat_mul(rot_z, m::quat_mul(rot_y, rot_x)));
}

bool make_pick_ray(const SceneCamera& camera,
                   float mouse_x,
                   float mouse_y,
                   float viewport_width,
                   float viewport_height,
                   EditorPickRay& out) noexcept {
    namespace m = psynder::math;
    if (!camera.active || viewport_width <= 1.0f || viewport_height <= 1.0f) {
        return false;
    }
    const float x_ndc = (mouse_x / viewport_width) * 2.0f - 1.0f;
    const float y_ndc = 1.0f - (mouse_y / viewport_height) * 2.0f;
    const float aspect = viewport_width / viewport_height;
    const float tan_half_fov = std::tan(
        std::clamp(camera.fov_y_deg, 1.0f, 179.0f) * m::kDegToRad * 0.5f);
    const m::Vec3 forward = camera_forward(camera);
    const m::Vec3 right = camera_right(camera);
    const m::Vec3 up = m::normalize(m::cross(right, forward));
    out.origin = camera.position;
    out.dir = m::normalize(m::add(
        forward,
        m::add(m::mul(right, x_ndc * tan_half_fov * aspect),
               m::mul(up, y_ndc * tan_half_fov))));
    return m::dot(out.dir, out.dir) > 0.0001f;
}

bool intersect_ray_local_aabb(psynder::math::Vec3 origin,
                              psynder::math::Vec3 dir,
                              psynder::math::Vec3 half_extent,
                              float& out_t) noexcept {
    float t_min = 0.0f;
    float t_max = std::numeric_limits<float>::max();
    const float origins[3] = {origin.x, origin.y, origin.z};
    const float dirs[3] = {dir.x, dir.y, dir.z};
    const float halfs[3] = {half_extent.x, half_extent.y, half_extent.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dirs[axis]) < 1.0e-6f) {
            if (origins[axis] < -halfs[axis] || origins[axis] > halfs[axis]) {
                return false;
            }
            continue;
        }
        float t0 = (-halfs[axis] - origins[axis]) / dirs[axis];
        float t1 = ( halfs[axis] - origins[axis]) / dirs[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        t_min = std::max(t_min, t0);
        t_max = std::min(t_max, t1);
        if (t_min > t_max) {
            return false;
        }
    }
    out_t = t_min;
    return true;
}

bool intersect_ray_local_sphere(psynder::math::Vec3 origin,
                                psynder::math::Vec3 dir,
                                float& out_t) noexcept {
    namespace m = psynder::math;
    const float a = m::dot(dir, dir);
    if (a <= 1.0e-8f) {
        return false;
    }
    const float b = 2.0f * m::dot(origin, dir);
    const float c = m::dot(origin, origin) - 0.25f;
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) {
        return false;
    }
    const float root = std::sqrt(disc);
    const float inv_2a = 0.5f / a;
    const float t0 = (-b - root) * inv_2a;
    const float t1 = (-b + root) * inv_2a;
    if (t0 >= 0.0f) {
        out_t = t0;
        return true;
    }
    if (t1 >= 0.0f) {
        out_t = t1;
        return true;
    }
    return false;
}

bool intersect_ray_primitive(const EditorPickRay& ray,
                             const ScenePrimitive& primitive,
                             float& out_t) noexcept {
    namespace m = psynder::math;
    if (!primitive.visible || primitive.name.empty()) {
        return false;
    }
    const m::Mat4 rotation = m::rotate_quat(primitive_rotation_quat(primitive));
    const m::Vec3 axis_x = m::normalize({rotation.m[0], rotation.m[1], rotation.m[2]});
    const m::Vec3 axis_y = m::normalize({rotation.m[4], rotation.m[5], rotation.m[6]});
    const m::Vec3 axis_z = m::normalize({rotation.m[8], rotation.m[9], rotation.m[10]});
    const m::Vec3 rel = m::sub(ray.origin, primitive.position);
    const m::Vec3 inv_scale{
        1.0f / std::max(std::fabs(primitive.scale.x), 1.0e-4f),
        1.0f / std::max(std::fabs(primitive.scale.y), 1.0e-4f),
        1.0f / std::max(std::fabs(primitive.scale.z), 1.0e-4f),
    };
    const m::Vec3 local_origin{
        m::dot(rel, axis_x) * inv_scale.x,
        m::dot(rel, axis_y) * inv_scale.y,
        m::dot(rel, axis_z) * inv_scale.z,
    };
    const m::Vec3 local_dir{
        m::dot(ray.dir, axis_x) * inv_scale.x,
        m::dot(ray.dir, axis_y) * inv_scale.y,
        m::dot(ray.dir, axis_z) * inv_scale.z,
    };
    if (primitive.kind == ScenePrimitiveKind::Sphere) {
        return intersect_ray_local_sphere(local_origin, local_dir, out_t);
    }
    const m::Vec3 half_extent = primitive.kind == ScenePrimitiveKind::Plane
        ? m::Vec3{0.5f, 0.025f, 0.5f}
        : m::Vec3{0.5f, 0.5f, 0.5f};
    return intersect_ray_local_aabb(local_origin, local_dir, half_extent, out_t);
}

bool intersect_ray_player_start(const EditorPickRay& ray,
                                const ScenePlayerStart& marker,
                                float& out_t) noexcept {
    namespace m = psynder::math;
    if (marker.name.empty()) {
        return false;
    }
    const m::Vec3 center = m::add(marker.position, {0.0f, 0.42f, 0.0f});
    const m::Vec3 local_origin = m::mul(m::sub(ray.origin, center), 1.0f / 0.55f);
    const m::Vec3 local_dir = m::mul(ray.dir, 1.0f / 0.55f);
    return intersect_ray_local_sphere(local_origin, local_dir, out_t);
}

std::string pick_scene_primitive(const PlayerBootState& boot,
                                 const SceneCamera& camera,
                                 float mouse_x,
                                 float mouse_y,
                                 float viewport_width,
                                 float viewport_height) {
    EditorPickRay ray{};
    if (boot.render_state != PlayerBootRenderState::SceneRendering ||
        !make_pick_ray(camera, mouse_x, mouse_y, viewport_width, viewport_height, ray)) {
        return {};
    }
    float best_t = std::numeric_limits<float>::max();
    std::string best_name;
    for (const ScenePrimitive& primitive : boot.primitives) {
        float t = 0.0f;
        if (intersect_ray_primitive(ray, primitive, t) && t >= 0.0f && t < best_t) {
            best_t = t;
            best_name = primitive.name;
        }
    }
    for (const ScenePlayerStart& marker : boot.player_starts) {
        float t = 0.0f;
        if (intersect_ray_player_start(ray, marker, t) && t >= 0.0f && t < best_t) {
            best_t = t;
            best_name = marker.name;
        }
    }
    for (const ScenePlayerController& controller : boot.player_controllers) {
        ScenePlayerStart marker{};
        marker.name = controller.name;
        marker.position = controller.position;
        marker.rotation_euler_deg = controller.rotation_euler_deg;
        float t = 0.0f;
        if (intersect_ray_player_start(ray, marker, t) && t >= 0.0f && t < best_t) {
            best_t = t;
            best_name = marker.name;
        }
    }
    return best_name;
}

bool selected_scene_transform(const PlayerBootState& boot,
                              SelectedSceneTransform& out) {
    for (const ScenePrimitive& primitive : boot.primitives) {
        if (primitive.selected && primitive.visible && !primitive.name.empty()) {
            out.name = primitive.name;
            out.position = primitive.position;
            out.rotation_euler_deg = primitive.rotation_euler_deg;
            out.scale = primitive.scale;
            out.allow_scale = true;
            return true;
        }
    }
    for (const ScenePlayerStart& player_start : boot.player_starts) {
        if (player_start.selected && !player_start.name.empty()) {
            out.name = player_start.name;
            out.position = player_start.position;
            out.rotation_euler_deg = player_start.rotation_euler_deg;
            out.scale = {1.0f, 1.0f, 1.0f};
            out.allow_scale = false;
            return true;
        }
    }
    for (const ScenePlayerController& controller : boot.player_controllers) {
        if (controller.selected && !controller.name.empty()) {
            out.name = controller.name;
            out.position = controller.position;
            out.rotation_euler_deg = controller.rotation_euler_deg;
            out.scale = {1.0f, 1.0f, 1.0f};
            out.allow_scale = false;
            return true;
        }
    }
    return false;
}

psynder::math::Vec3 gizmo_axis_vector(GizmoAxis axis) noexcept {
    switch (axis) {
        case GizmoAxis::X: return {1.0f, 0.0f, 0.0f};
        case GizmoAxis::Y: return {0.0f, 1.0f, 0.0f};
        case GizmoAxis::Z: return {0.0f, 0.0f, 1.0f};
        case GizmoAxis::PlaneXY:
        case GizmoAxis::PlaneXZ:
        case GizmoAxis::PlaneYZ:
        case GizmoAxis::None: break;
    }
    return {0.0f, 0.0f, 0.0f};
}

psynder::math::Vec3 gizmo_rotation_axis_vector(GizmoAxis axis) noexcept {
    switch (axis) {
        case GizmoAxis::PlaneXY: return {0.0f, 0.0f, 1.0f};
        case GizmoAxis::PlaneXZ: return {0.0f, 1.0f, 0.0f};
        case GizmoAxis::PlaneYZ: return {1.0f, 0.0f, 0.0f};
        case GizmoAxis::X:
        case GizmoAxis::Y:
        case GizmoAxis::Z:
            return gizmo_axis_vector(axis);
        case GizmoAxis::None:
            break;
    }
    return {0.0f, 0.0f, 0.0f};
}

bool gizmo_plane_basis(GizmoAxis axis,
                       psynder::math::Vec3& out_u,
                       psynder::math::Vec3& out_v,
                       psynder::math::Vec3& out_normal) noexcept {
    switch (axis) {
        case GizmoAxis::PlaneXY:
            out_u = {1.0f, 0.0f, 0.0f};
            out_v = {0.0f, 1.0f, 0.0f};
            out_normal = {0.0f, 0.0f, 1.0f};
            return true;
        case GizmoAxis::PlaneXZ:
            out_u = {1.0f, 0.0f, 0.0f};
            out_v = {0.0f, 0.0f, 1.0f};
            out_normal = {0.0f, 1.0f, 0.0f};
            return true;
        case GizmoAxis::PlaneYZ:
            out_u = {0.0f, 1.0f, 0.0f};
            out_v = {0.0f, 0.0f, 1.0f};
            out_normal = {1.0f, 0.0f, 0.0f};
            return true;
        case GizmoAxis::X:
        case GizmoAxis::Y:
        case GizmoAxis::Z:
        case GizmoAxis::None:
            break;
    }
    return false;
}

bool gizmo_axis_drag_plane(const SceneCamera& camera,
                           GizmoAxis axis,
                           psynder::math::Vec3& out_normal) noexcept {
    namespace m = psynder::math;
    const m::Vec3 axis_dir = gizmo_axis_vector(axis);
    if (m::dot(axis_dir, axis_dir) <= 0.0001f) {
        return false;
    }
    const auto make_normal = [&](m::Vec3 candidate) {
        return m::sub(candidate, m::mul(axis_dir, m::dot(candidate, axis_dir)));
    };
    out_normal = make_normal(camera_forward(camera));
    if (m::length(out_normal) < 0.08f) {
        out_normal = make_normal(camera_right(camera));
    }
    if (m::length(out_normal) < 0.08f) {
        out_normal = make_normal({0.0f, 1.0f, 0.0f});
    }
    if (m::length(out_normal) < 0.08f) {
        return false;
    }
    out_normal = m::normalize(out_normal);
    return true;
}

bool intersect_ray_plane(const EditorPickRay& ray,
                         psynder::math::Vec3 point,
                         psynder::math::Vec3 normal,
                         psynder::math::Vec3& out_hit) noexcept {
    namespace m = psynder::math;
    const float denom = m::dot(ray.dir, normal);
    if (std::fabs(denom) < 1.0e-5f) {
        return false;
    }
    const float t = m::dot(m::sub(point, ray.origin), normal) / denom;
    if (t < 0.0f || !std::isfinite(t)) {
        return false;
    }
    out_hit = m::add(ray.origin, m::mul(ray.dir, t));
    return true;
}

psynder::math::Vec3 apply_grid_snap_delta(psynder::math::Vec3 delta,
                                          const EditorSnapSettings& snap) noexcept {
    if (!snap.enabled || snap.step <= 0.0f) {
        return delta;
    }
    const float inv_step = 1.0f / snap.step;
    return {
        std::round(delta.x * inv_step) * snap.step,
        std::round(delta.y * inv_step) * snap.step,
        std::round(delta.z * inv_step) * snap.step,
    };
}

float gizmo_world_scale(const SceneCamera& camera,
                        psynder::math::Vec3 origin) noexcept {
    namespace m = psynder::math;
    const float distance = m::length(m::sub(camera.position, origin));
    return std::clamp(distance * 0.16f, 0.55f, 2.5f);
}

struct GizmoBox {
    GizmoAxis axis = GizmoAxis::None;
    psynder::math::Vec3 center{};
    psynder::math::Vec3 half_extent{};
    psynder::math::Vec3 pick_half_extent{};
    psynder::math::Vec3 render_half_extent{};
    psynder::math::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    bool arrow_head = false;
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

constexpr std::size_t kMaxGizmoBoxes = 96;

psynder::math::Quat gizmo_rotation_from_x_to(psynder::math::Vec3 dir) noexcept {
    namespace m = psynder::math;
    const m::Vec3 to = m::normalize(dir);
    if (m::dot(to, to) <= 0.0001f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const m::Vec3 from{1.0f, 0.0f, 0.0f};
    const float d = std::clamp(m::dot(from, to), -1.0f, 1.0f);
    if (d > 0.9999f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    if (d < -0.9999f) {
        return m::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, m::kPi);
    }
    const m::Vec3 c = m::cross(from, to);
    return m::quat_normalize({c.x, c.y, c.z, 1.0f + d});
}

psynder::math::Quat gizmo_box_render_rotation(const GizmoBox& box) noexcept {
    namespace m = psynder::math;
    if (box.arrow_head) {
        if (box.axis == GizmoAxis::Y) {
            return m::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, m::kHalfPi);
        }
        if (box.axis == GizmoAxis::Z) {
            return m::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -m::kHalfPi);
        }
    }
    return box.rotation;
}

psynder::math::Vec3 rotate_vec_by_quat(psynder::math::Quat q,
                                       psynder::math::Vec3 v) noexcept {
    namespace m = psynder::math;
    const m::Vec3 qv{q.x, q.y, q.z};
    const m::Vec3 t = m::mul(m::cross(qv, v), 2.0f);
    return m::add(m::add(v, m::mul(t, q.w)), m::cross(qv, t));
}

psynder::math::Quat quat_conjugate_local(psynder::math::Quat q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}

void build_translate_gizmo_boxes(psynder::math::Vec3 origin,
                                 float scale,
                                 GizmoAxis active_axis,
                                 GizmoAxis hover_axis,
                                 GizmoBox (&out)[kMaxGizmoBoxes],
                                 std::size_t& out_count) noexcept {
    namespace m = psynder::math;
    out_count = 0;
    const auto inflate = [](m::Vec3 half_extent, float pad) noexcept {
        return m::Vec3{
            half_extent.x + pad,
            half_extent.y + pad,
            half_extent.z + pad,
        };
    };
    const auto push = [&](GizmoAxis axis,
                          m::Vec3 center,
                          m::Vec3 half_extent,
                          m::Vec3 render_half_extent,
                          float r,
                          float g,
                          float b,
                          float alpha,
                          bool arrow_head = false) {
        if (out_count >= kMaxGizmoBoxes) {
            return;
        }
        GizmoBox box{};
        box.axis = axis;
        box.center = center;
        box.half_extent = half_extent;
        const bool plane =
            axis == GizmoAxis::PlaneXY ||
            axis == GizmoAxis::PlaneXZ ||
            axis == GizmoAxis::PlaneYZ;
        const bool highlighted = axis == active_axis || axis == hover_axis;
        box.pick_half_extent =
            plane ? inflate(half_extent, 0.055f * scale)
                  : inflate(half_extent, arrow_head ? 0.045f * scale : 0.035f * scale);
        const float visual_scale = highlighted ? 1.08f : 1.0f;
        box.render_half_extent = m::mul(render_half_extent, visual_scale);
        box.arrow_head = arrow_head;
        const float boost =
            axis == active_axis ? 1.38f :
            axis == hover_axis ? 1.18f :
            1.0f;
        box.tint[0] = std::min(r * boost, 1.6f);
        box.tint[1] = std::min(g * boost, 1.6f);
        box.tint[2] = std::min(b * boost, 1.6f);
        box.tint[3] = axis == active_axis ? std::min(alpha + 0.18f, 1.0f) :
                      axis == hover_axis ? std::min(alpha + 0.10f, 1.0f) :
                      alpha;
        out[out_count++] = box;
    };

    const float shaft_len = 1.22f * scale;
    const float shaft_radius = 0.042f * scale;
    const float head_len = 0.20f * scale;
    const float head_radius = 0.15f * scale;
    const float head_offset = 1.28f * scale;
    const float plane_offset = 0.42f * scale;
    const float plane_size = 0.25f * scale;
    const float plane_thickness = 0.014f * scale;
    push(GizmoAxis::X,
         m::add(origin, {shaft_len * 0.5f, 0.0f, 0.0f}),
         {shaft_len * 0.5f, shaft_radius, shaft_radius},
         {shaft_len * 0.5f, shaft_radius, shaft_radius},
         1.0f, 0.18f, 0.16f, 0.82f);
    push(GizmoAxis::X,
         m::add(origin, {head_offset, 0.0f, 0.0f}),
         {head_len, head_radius, head_radius},
         {head_len, head_radius, head_radius},
         1.0f, 0.28f, 0.18f, 0.96f, true);
    push(GizmoAxis::Y,
         m::add(origin, {0.0f, shaft_len * 0.5f, 0.0f}),
         {shaft_radius, shaft_len * 0.5f, shaft_radius},
         {shaft_radius, shaft_len * 0.5f, shaft_radius},
         0.24f, 1.0f, 0.30f, 0.82f);
    push(GizmoAxis::Y,
         m::add(origin, {0.0f, head_offset, 0.0f}),
         {head_radius, head_len, head_radius},
         {head_len, head_radius, head_radius},
         0.35f, 1.0f, 0.35f, 0.96f, true);
    push(GizmoAxis::Z,
         m::add(origin, {0.0f, 0.0f, shaft_len * 0.5f}),
         {shaft_radius, shaft_radius, shaft_len * 0.5f},
         {shaft_radius, shaft_radius, shaft_len * 0.5f},
         0.24f, 0.52f, 1.0f, 0.82f);
    push(GizmoAxis::Z,
         m::add(origin, {0.0f, 0.0f, head_offset}),
         {head_radius, head_radius, head_len},
         {head_len, head_radius, head_radius},
         0.34f, 0.62f, 1.0f, 0.96f, true);
    push(GizmoAxis::PlaneXY,
         m::add(origin, {plane_offset, plane_offset, 0.0f}),
         {plane_size, plane_size, plane_thickness},
         {plane_size, plane_size, plane_thickness},
         1.0f, 0.94f, 0.28f, 0.32f);
    push(GizmoAxis::PlaneXZ,
         m::add(origin, {plane_offset, 0.0f, plane_offset}),
         {plane_size, plane_thickness, plane_size},
         {plane_size, plane_thickness, plane_size},
         0.95f, 0.22f, 1.0f, 0.32f);
    push(GizmoAxis::PlaneYZ,
         m::add(origin, {0.0f, plane_offset, plane_offset}),
         {plane_thickness, plane_size, plane_size},
         {plane_thickness, plane_size, plane_size},
         0.20f, 0.90f, 1.0f, 0.32f);
}

void build_scale_gizmo_boxes(psynder::math::Vec3 origin,
                             float scale,
                             GizmoAxis active_axis,
                             GizmoAxis hover_axis,
                             GizmoBox (&out)[kMaxGizmoBoxes],
                             std::size_t& out_count) noexcept {
    namespace m = psynder::math;
    out_count = 0;
    const auto inflate = [](m::Vec3 half_extent, float pad) noexcept {
        return m::Vec3{
            half_extent.x + pad,
            half_extent.y + pad,
            half_extent.z + pad,
        };
    };
    const auto push = [&](GizmoAxis axis,
                          m::Vec3 center,
                          m::Vec3 half_extent,
                          float r,
                          float g,
                          float b,
                          float alpha) {
        if (out_count >= kMaxGizmoBoxes) {
            return;
        }
        GizmoBox box{};
        box.axis = axis;
        box.center = center;
        box.half_extent = half_extent;
        const bool plane =
            axis == GizmoAxis::PlaneXY ||
            axis == GizmoAxis::PlaneXZ ||
            axis == GizmoAxis::PlaneYZ;
        const bool highlighted = axis == active_axis || axis == hover_axis;
        box.pick_half_extent =
            plane ? inflate(half_extent, 0.060f * scale)
                  : inflate(half_extent, 0.040f * scale);
        box.render_half_extent = m::mul(half_extent, highlighted ? 1.08f : 1.0f);
        const float boost =
            axis == active_axis ? 1.42f :
            axis == hover_axis ? 1.20f :
            1.0f;
        box.tint[0] = std::min(r * boost, 1.65f);
        box.tint[1] = std::min(g * boost, 1.65f);
        box.tint[2] = std::min(b * boost, 1.65f);
        box.tint[3] = axis == active_axis ? std::min(alpha + 0.20f, 1.0f) :
                      axis == hover_axis ? std::min(alpha + 0.12f, 1.0f) :
                      alpha;
        out[out_count++] = box;
    };

    const float rail_len = 1.16f * scale;
    const float rail_radius = 0.036f * scale;
    const float cube_size = 0.17f * scale;
    const float cube_offset = 1.22f * scale;
    const float plane_offset = 0.40f * scale;
    const float plane_size = 0.23f * scale;
    const float plane_thickness = 0.014f * scale;
    push(GizmoAxis::X,
         m::add(origin, {rail_len * 0.5f, 0.0f, 0.0f}),
         {rail_len * 0.5f, rail_radius, rail_radius},
         1.0f, 0.20f, 0.18f, 0.78f);
    push(GizmoAxis::X,
         m::add(origin, {cube_offset, 0.0f, 0.0f}),
         {cube_size, cube_size, cube_size},
         1.0f, 0.32f, 0.22f, 0.98f);
    push(GizmoAxis::Y,
         m::add(origin, {0.0f, rail_len * 0.5f, 0.0f}),
         {rail_radius, rail_len * 0.5f, rail_radius},
         0.26f, 1.0f, 0.32f, 0.78f);
    push(GizmoAxis::Y,
         m::add(origin, {0.0f, cube_offset, 0.0f}),
         {cube_size, cube_size, cube_size},
         0.36f, 1.0f, 0.36f, 0.98f);
    push(GizmoAxis::Z,
         m::add(origin, {0.0f, 0.0f, rail_len * 0.5f}),
         {rail_radius, rail_radius, rail_len * 0.5f},
         0.26f, 0.56f, 1.0f, 0.78f);
    push(GizmoAxis::Z,
         m::add(origin, {0.0f, 0.0f, cube_offset}),
         {cube_size, cube_size, cube_size},
         0.36f, 0.66f, 1.0f, 0.98f);
    push(GizmoAxis::PlaneXY,
         m::add(origin, {plane_offset, plane_offset, 0.0f}),
         {plane_size, plane_size, plane_thickness},
         1.0f, 0.94f, 0.34f, 0.38f);
    push(GizmoAxis::PlaneXZ,
         m::add(origin, {plane_offset, 0.0f, plane_offset}),
         {plane_size, plane_thickness, plane_size},
         0.95f, 0.30f, 1.0f, 0.38f);
    push(GizmoAxis::PlaneYZ,
         m::add(origin, {0.0f, plane_offset, plane_offset}),
         {plane_thickness, plane_size, plane_size},
         0.26f, 0.92f, 1.0f, 0.38f);
}

void build_rotate_gizmo_boxes(psynder::math::Vec3 origin,
                              float scale,
                              GizmoAxis active_axis,
                              GizmoAxis hover_axis,
                              GizmoBox (&out)[kMaxGizmoBoxes],
                              std::size_t& out_count) noexcept {
    namespace m = psynder::math;
    out_count = 0;
    const auto push = [&](GizmoAxis axis,
                          m::Vec3 center,
                          m::Vec3 tangent,
                          float r,
                          float g,
                          float b) {
        if (out_count >= kMaxGizmoBoxes) {
            return;
        }
        GizmoBox box{};
        box.axis = axis;
        box.center = center;
        const float boost =
            axis == active_axis ? 1.45f :
            axis == hover_axis ? 1.22f :
            1.0f;
        const bool highlighted = axis == active_axis || axis == hover_axis;
        const float seg_len = (highlighted ? 0.22f : 0.19f) * scale;
        const float seg_radius = (highlighted ? 0.030f : 0.024f) * scale;
        const float pick_radius = 0.125f * scale;
        box.half_extent = {pick_radius, pick_radius, pick_radius};
        box.pick_half_extent = box.half_extent;
        box.render_half_extent = {seg_len, seg_radius, seg_radius};
        box.rotation = gizmo_rotation_from_x_to(tangent);
        box.tint[0] = std::min(r * boost, 1.65f);
        box.tint[1] = std::min(g * boost, 1.65f);
        box.tint[2] = std::min(b * boost, 1.65f);
        box.tint[3] = axis == active_axis ? 1.0f :
                      axis == hover_axis ? 0.92f :
                      0.74f;
        out[out_count++] = box;
    };

    constexpr int kSegments = 24;
    const float radius = 0.92f * scale;
    for (int i = 0; i < kSegments; ++i) {
        const float a = (static_cast<float>(i) + 0.5f) *
                        (m::kTwoPi / static_cast<float>(kSegments));
        const float c = std::cos(a);
        const float s = std::sin(a);
        push(GizmoAxis::X,
             m::add(origin, {0.0f, c * radius, s * radius}),
             {0.0f, -s, c},
             1.0f, 0.18f, 0.16f);
        push(GizmoAxis::Y,
             m::add(origin, {c * radius, 0.0f, s * radius}),
             {-s, 0.0f, c},
             0.24f, 1.0f, 0.30f);
        push(GizmoAxis::Z,
             m::add(origin, {c * radius, s * radius, 0.0f}),
             {-s, c, 0.0f},
             0.24f, 0.52f, 1.0f);
    }
}

void build_runtime_gizmo_boxes(TransformMode mode,
                               psynder::math::Vec3 origin,
                               float scale,
                               GizmoAxis active_axis,
                               GizmoAxis hover_axis,
                               GizmoBox (&out)[kMaxGizmoBoxes],
                               std::size_t& out_count) noexcept {
    switch (mode) {
        case TransformMode::Rotate:
            build_rotate_gizmo_boxes(origin, scale, active_axis, hover_axis, out, out_count);
            return;
        case TransformMode::Scale:
            build_scale_gizmo_boxes(origin, scale, active_axis, hover_axis, out, out_count);
            return;
        case TransformMode::Translate:
            break;
    }
    build_translate_gizmo_boxes(origin, scale, active_axis, hover_axis, out, out_count);
}

bool intersect_ray_world_box(const EditorPickRay& ray,
                             const GizmoBox& box,
                             float& out_t) noexcept {
    namespace m = psynder::math;
    const m::Quat inv_rotation =
        quat_conjugate_local(m::quat_normalize(gizmo_box_render_rotation(box)));
    const m::Vec3 local_origin =
        rotate_vec_by_quat(inv_rotation, m::sub(ray.origin, box.center));
    const m::Vec3 local_dir = rotate_vec_by_quat(inv_rotation, ray.dir);
    return intersect_ray_local_aabb(local_origin, local_dir, box.pick_half_extent, out_t);
}

GizmoAxis pick_runtime_gizmo(const PlayerBootState& boot,
                             const SceneCamera& camera,
                             TransformMode mode,
                             float mouse_x,
                             float mouse_y,
                             float viewport_width,
                             float viewport_height) {
    SelectedSceneTransform selected{};
    if (!selected_scene_transform(boot, selected) ||
        (mode == TransformMode::Scale && !selected.allow_scale)) {
        return GizmoAxis::None;
    }
    EditorPickRay ray{};
    if (!make_pick_ray(camera, mouse_x, mouse_y, viewport_width, viewport_height, ray)) {
        return GizmoAxis::None;
    }

    GizmoBox boxes[kMaxGizmoBoxes]{};
    std::size_t box_count = 0;
    build_runtime_gizmo_boxes(mode,
                              selected.position,
                              gizmo_world_scale(camera, selected.position),
                              GizmoAxis::None,
                              GizmoAxis::None,
                              boxes,
                              box_count);
    float best_t = std::numeric_limits<float>::max();
    GizmoAxis best_axis = GizmoAxis::None;
    for (std::size_t i = 0; i < box_count; ++i) {
        float t = 0.0f;
        if (intersect_ray_world_box(ray, boxes[i], t) && t >= 0.0f && t < best_t) {
            best_t = t;
            best_axis = boxes[i].axis;
        }
    }
    return best_axis;
}

bool begin_gizmo_drag(GizmoDragState& drag,
                      const PlayerBootState& boot,
                      const SceneCamera& camera,
                      TransformMode mode,
                      GizmoAxis axis,
                      float mouse_x,
                      float mouse_y,
                      float viewport_width,
                      float viewport_height) {
    SelectedSceneTransform selected{};
    if (!selected_scene_transform(boot, selected) ||
        axis == GizmoAxis::None ||
        (mode == TransformMode::Scale && !selected.allow_scale)) {
        return false;
    }
    EditorPickRay ray{};
    if (!make_pick_ray(camera, mouse_x, mouse_y, viewport_width, viewport_height, ray)) {
        return false;
    }
    psynder::math::Vec3 hit{};
    psynder::math::Vec3 plane_normal{};
    psynder::math::Vec3 plane_u{};
    psynder::math::Vec3 plane_v{};
    float param = 0.0f;
    if (gizmo_plane_basis(axis, plane_u, plane_v, plane_normal)) {
        if (!intersect_ray_plane(ray, selected.position, plane_normal, hit)) {
            return false;
        }
    } else if (gizmo_axis_drag_plane(camera, axis, plane_normal)) {
        if (!intersect_ray_plane(ray, selected.position, plane_normal, hit)) {
            return false;
        }
        param = psynder::math::dot(
            psynder::math::sub(hit, selected.position),
            gizmo_axis_vector(axis));
    } else {
        return false;
    }
    drag.active = true;
    drag.mode = mode;
    drag.axis = axis;
    drag.entity_name = selected.name;
    drag.allow_scale = selected.allow_scale;
    drag.start_position = selected.position;
    drag.start_rotation_euler_deg = selected.rotation_euler_deg;
    drag.start_scale = selected.scale;
    drag.start_plane_point = hit;
    drag.plane_normal = plane_normal;
    drag.plane_u = plane_u;
    drag.plane_v = plane_v;
    drag.start_axis_param = param;
    drag.start_mouse_x = mouse_x;
    drag.start_mouse_y = mouse_y;
    return true;
}

bool update_gizmo_drag(GizmoDragState& drag,
                       PlayerBootState& boot,
                       const SceneCamera& camera,
                       float mouse_x,
                       float mouse_y,
                       float viewport_width,
                       float viewport_height) {
    namespace m = psynder::math;
    if (!drag.active || drag.axis == GizmoAxis::None || drag.entity_name.empty()) {
        return false;
    }
    EditorPickRay ray{};
    m::Vec3 hit{};
    if (!make_pick_ray(camera, mouse_x, mouse_y, viewport_width, viewport_height, ray) ||
        !intersect_ray_plane(ray, drag.start_position, drag.plane_normal, hit)) {
        return false;
    }
    m::Vec3 delta{};
    if (gizmo_plane_basis(drag.axis, drag.plane_u, drag.plane_v, drag.plane_normal)) {
        const m::Vec3 raw = m::sub(hit, drag.start_plane_point);
        delta = m::add(m::mul(drag.plane_u, m::dot(raw, drag.plane_u)),
                       m::mul(drag.plane_v, m::dot(raw, drag.plane_v)));
    } else {
        const float param = m::dot(m::sub(hit, drag.start_position),
                                  gizmo_axis_vector(drag.axis));
        delta = m::mul(gizmo_axis_vector(drag.axis), param - drag.start_axis_param);
    }
    if (drag.mode == TransformMode::Rotate) {
        const m::Vec3 axis = gizmo_rotation_axis_vector(drag.axis);
        if (m::dot(axis, axis) <= 0.0001f) {
            return false;
        }
        const float angle_deg =
            ((mouse_x - drag.start_mouse_x) - (mouse_y - drag.start_mouse_y)) * 0.35f;
        m::Vec3 rotation = drag.start_rotation_euler_deg;
        rotation.x += axis.x * angle_deg;
        rotation.y += axis.y * angle_deg;
        rotation.z += axis.z * angle_deg;
        return sync_editor_entity_vec3(boot,
                                       drag.entity_name,
                                       "rotation_euler_deg",
                                       rotation,
                                       "gizmo drag update");
    }
    if (drag.mode == TransformMode::Scale) {
        if (!drag.allow_scale) {
            return false;
        }
        m::Vec3 scale = m::add(drag.start_scale, delta);
        scale.x = std::max(scale.x, 0.001f);
        scale.y = std::max(scale.y, 0.001f);
        scale.z = std::max(scale.z, 0.001f);
        return sync_editor_entity_vec3(boot,
                                       drag.entity_name,
                                       "scale",
                                       scale,
                                       "gizmo drag update");
    }
    const m::Vec3 snapped_delta =
        apply_grid_snap_delta(delta, parse_editor_snap_settings(boot.scene_document_json));
    return sync_editor_entity_position(boot,
                                       drag.entity_name,
                                       m::add(drag.start_position, snapped_delta),
                                       "gizmo drag update");
}

psynder::Entity create_fallback_crate(psynder::scene::World& world) {
    const psynder::Entity crate = world.create();
    world.add(crate, crate_transform(0.0f));
    world.add(crate, psynder::scene::VisibleBit{
        psynder::scene::VisibleBit::Partition::WorldDynamic, 0, 0});
    return crate;
}

psynder::Entity create_script_crate(psynder::scene::World& world,
                                    bool script_started) {
    if (script_started) {
        constexpr std::string_view kSpawnCrate = R"(
crate_engine_entity = select(2, world:create_entity({
  TransformWS = { position = {0.0, 0.5, 0.0} },
  VisibleBit = { partition = 'dynamic' },
  MeshRef = { mesh = 1, lod_bias = 0 },
  MaterialRef = { material = 1 },
}))
)";
        std::string out;
        if (psynder::script::Vm::Get().execute_string(kSpawnCrate,
                                                       PSYNDER_GX_APP_LOG_PREFIX "_spawn") &&
            psynder::script::Vm::Get().execute_repl("crate_engine_entity", out)) {
            char* end = nullptr;
            const unsigned long raw = std::strtoul(out.c_str(), &end, 10);
            if (end != out.c_str() && raw <= 0xFFFF'FFFFul) {
                const psynder::Entity crate{static_cast<psynder::u32>(raw)};
                if (world.alive(crate)) {
                    return crate;
                }
            }
        }
    }

    return create_fallback_crate(world);
}

constexpr PrimitiveVertex make_vertex(float x,
                                      float y,
                                      float z,
                                      float nx,
                                      float ny,
                                      float nz,
                                      float u,
                                      float v,
                                      float r,
                                      float g,
                                      float b) noexcept {
    return {{x, y, z}, {nx, ny, nz}, {u, v}, {r, g, b, 1.0f}};
}

bool upload_mesh(psynder::gpu::Device* dev,
                 PrimitiveMeshGpu& out,
                 const PrimitiveVertex* vertices,
                 std::size_t vertex_count,
                 const std::uint16_t* indices,
                 std::size_t index_count,
                 const char* debug_name) {
    namespace g = psynder::gpu;
    g::BufferDesc vb{};
    vb.size_bytes = sizeof(PrimitiveVertex) * vertex_count;
    vb.usage = static_cast<std::uint32_t>(g::BufferUsage::Vertex);
    vb.heap = g::device_is_unified_memory(dev) ? g::HeapKind::DeviceLocal : g::HeapKind::HostVisible;
    vb.debug_name = debug_name;
    out.vertex_buffer = g::create_buffer(dev, vb);
    if (!out.vertex_buffer) {
        return false;
    }
    if (void* mapped = g::buffer_map(out.vertex_buffer.get())) {
        std::memcpy(mapped, vertices, vb.size_bytes);
        g::buffer_unmap(out.vertex_buffer.get());
    }

    g::BufferDesc ib{};
    ib.size_bytes = sizeof(std::uint16_t) * index_count;
    ib.usage = static_cast<std::uint32_t>(g::BufferUsage::Index);
    ib.heap = g::device_is_unified_memory(dev) ? g::HeapKind::DeviceLocal : g::HeapKind::HostVisible;
    ib.debug_name = debug_name;
    out.index_buffer = g::create_buffer(dev, ib);
    if (!out.index_buffer) {
        return false;
    }
    if (void* mapped = g::buffer_map(out.index_buffer.get())) {
        std::memcpy(mapped, indices, ib.size_bytes);
        g::buffer_unmap(out.index_buffer.get());
    }
    out.index_count = static_cast<std::uint32_t>(index_count);
    return true;
}

std::vector<PrimitiveVertex> build_sphere_vertices(std::uint32_t rings,
                                                   std::uint32_t segments) {
    std::vector<PrimitiveVertex> vertices;
    vertices.reserve((rings + 1u) * (segments + 1u));
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float phi = v * psynder::math::kPi;
        const float y = std::cos(phi) * 0.5f;
        const float rr = std::sin(phi) * 0.5f;
        for (std::uint32_t s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float theta = u * psynder::math::kTwoPi;
            const float x = std::cos(theta) * rr;
            const float z = std::sin(theta) * rr;
            const psynder::math::Vec3 n = psynder::math::normalize({x, y, z});
            vertices.push_back(make_vertex(x, y, z, n.x, n.y, n.z, u, v, 0.45f, 0.78f, 1.0f));
        }
    }
    return vertices;
}

std::vector<std::uint16_t> build_sphere_indices(std::uint32_t rings,
                                                std::uint32_t segments) {
    std::vector<std::uint16_t> indices;
    indices.reserve(rings * segments * 6u);
    const std::uint32_t stride = segments + 1u;
    for (std::uint32_t r = 0; r < rings; ++r) {
        for (std::uint32_t s = 0; s < segments; ++s) {
            const std::uint32_t a = r * stride + s;
            const std::uint32_t b = a + 1u;
            const std::uint32_t c = a + stride;
            const std::uint32_t d = c + 1u;
            indices.push_back(static_cast<std::uint16_t>(a));
            indices.push_back(static_cast<std::uint16_t>(c));
            indices.push_back(static_cast<std::uint16_t>(b));
            indices.push_back(static_cast<std::uint16_t>(b));
            indices.push_back(static_cast<std::uint16_t>(c));
            indices.push_back(static_cast<std::uint16_t>(d));
        }
    }
    return indices;
}

// A capsule = a sphere of `radius` split at the equator and pulled apart by
// `cyl_height`, so two hemispheres cap a cylindrical middle. Total height is
// 2*radius + cyl_height, centred at the origin. Topology matches the sphere, so
// build_sphere_indices(rings, segments) indexes it unchanged.
std::vector<PrimitiveVertex> build_capsule_vertices(std::uint32_t rings,
                                                    std::uint32_t segments,
                                                    float radius, float cyl_height) {
    std::vector<PrimitiveVertex> vertices;
    vertices.reserve((rings + 1u) * (segments + 1u));
    const float half_cyl = cyl_height * 0.5f;
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float phi = v * psynder::math::kPi;
        const float sy = std::cos(phi) * radius;  // sphere-space y (hemisphere)
        const float rr = std::sin(phi) * radius;
        const float yoff = (sy >= 0.0f) ? half_cyl : -half_cyl;
        for (std::uint32_t s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float theta = u * psynder::math::kTwoPi;
            const float x = std::cos(theta) * rr;
            const float z = std::sin(theta) * rr;
            const psynder::math::Vec3 n = psynder::math::normalize({x, sy, z});
            vertices.push_back(make_vertex(x, sy + yoff, z, n.x, n.y, n.z, u, v,
                                           0.45f, 0.78f, 1.0f));
        }
    }
    return vertices;
}

bool init_primitive_renderer(psynder::gpu::Device* dev,
                             PrimitiveRenderResources& out,
                             bool quiet) {
    namespace g = psynder::gpu;
    namespace sh = psynder::shader;

    constexpr PrimitiveVertex kCubeVertices[] = {
        make_vertex(-0.5f,-0.5f, 0.5f, 0,0,1, 0,1, 0.75f,0.90f,1.00f),
        make_vertex( 0.5f,-0.5f, 0.5f, 0,0,1, 1,1, 0.75f,0.90f,1.00f),
        make_vertex( 0.5f, 0.5f, 0.5f, 0,0,1, 1,0, 0.75f,0.90f,1.00f),
        make_vertex(-0.5f, 0.5f, 0.5f, 0,0,1, 0,0, 0.75f,0.90f,1.00f),
        make_vertex( 0.5f,-0.5f,-0.5f, 1,0,0, 0,1, 0.55f,0.82f,1.00f),
        make_vertex( 0.5f, 0.5f,-0.5f, 1,0,0, 1,1, 0.55f,0.82f,1.00f),
        make_vertex( 0.5f, 0.5f, 0.5f, 1,0,0, 1,0, 0.55f,0.82f,1.00f),
        make_vertex( 0.5f,-0.5f, 0.5f, 1,0,0, 0,0, 0.55f,0.82f,1.00f),
        make_vertex(-0.5f,-0.5f,-0.5f, 0,0,-1, 0,1, 0.55f,1.00f,0.68f),
        make_vertex(-0.5f, 0.5f,-0.5f, 0,0,-1, 1,1, 0.55f,1.00f,0.68f),
        make_vertex( 0.5f, 0.5f,-0.5f, 0,0,-1, 1,0, 0.55f,1.00f,0.68f),
        make_vertex( 0.5f,-0.5f,-0.5f, 0,0,-1, 0,0, 0.55f,1.00f,0.68f),
        make_vertex(-0.5f,-0.5f,-0.5f, -1,0,0, 0,1, 0.95f,0.82f,0.46f),
        make_vertex(-0.5f,-0.5f, 0.5f, -1,0,0, 1,1, 0.95f,0.82f,0.46f),
        make_vertex(-0.5f, 0.5f, 0.5f, -1,0,0, 1,0, 0.95f,0.82f,0.46f),
        make_vertex(-0.5f, 0.5f,-0.5f, -1,0,0, 0,0, 0.95f,0.82f,0.46f),
        make_vertex(-0.5f, 0.5f, 0.5f, 0,1,0, 0,1, 0.80f,0.62f,1.00f),
        make_vertex( 0.5f, 0.5f, 0.5f, 0,1,0, 1,1, 0.80f,0.62f,1.00f),
        make_vertex( 0.5f, 0.5f,-0.5f, 0,1,0, 1,0, 0.80f,0.62f,1.00f),
        make_vertex(-0.5f, 0.5f,-0.5f, 0,1,0, 0,0, 0.80f,0.62f,1.00f),
        make_vertex(-0.5f,-0.5f,-0.5f, 0,-1,0, 0,1, 1.00f,0.55f,0.55f),
        make_vertex( 0.5f,-0.5f,-0.5f, 0,-1,0, 1,1, 1.00f,0.55f,0.55f),
        make_vertex( 0.5f,-0.5f, 0.5f, 0,-1,0, 1,0, 1.00f,0.55f,0.55f),
        make_vertex(-0.5f,-0.5f, 0.5f, 0,-1,0, 0,0, 1.00f,0.55f,0.55f),
    };
    constexpr std::uint16_t kCubeIndices[] = {
        0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
        12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23,
    };
    constexpr PrimitiveVertex kPlaneVertices[] = {
        make_vertex(-0.5f,0.0f,-0.5f, 0,1,0, 0,1, 0.32f,0.42f,0.48f),
        make_vertex( 0.5f,0.0f,-0.5f, 0,1,0, 1,1, 0.32f,0.42f,0.48f),
        make_vertex( 0.5f,0.0f, 0.5f, 0,1,0, 1,0, 0.32f,0.42f,0.48f),
        make_vertex(-0.5f,0.0f, 0.5f, 0,1,0, 0,0, 0.32f,0.42f,0.48f),
    };
    constexpr std::uint16_t kPlaneIndices[] = {0, 1, 2, 0, 2, 3};
    constexpr PrimitiveVertex kGridVertices[] = {
        make_vertex(-0.5f,0.0f,-0.5f, 0,1,0, 0,1, 0.18f,0.26f,0.30f),
        make_vertex( 0.5f,0.0f,-0.5f, 0,1,0, 1,1, 0.18f,0.26f,0.30f),
        make_vertex( 0.5f,0.0f, 0.5f, 0,1,0, 1,0, 0.18f,0.26f,0.30f),
        make_vertex(-0.5f,0.0f, 0.5f, 0,1,0, 0,0, 0.18f,0.26f,0.30f),
    };
    constexpr std::uint16_t kGridIndices[] = {0, 1, 2, 0, 2, 3};
    constexpr PrimitiveVertex kSkyVertices[] = {
        make_vertex(-1.0f,-1.0f, 0.0f, 0,0,1, 0,1, 1.0f,1.0f,1.0f),
        make_vertex( 1.0f,-1.0f, 0.0f, 0,0,1, 1,1, 1.0f,1.0f,1.0f),
        make_vertex( 1.0f, 1.0f, 0.0f, 0,0,1, 1,0, 1.0f,1.0f,1.0f),
        make_vertex(-1.0f, 1.0f, 0.0f, 0,0,1, 0,0, 1.0f,1.0f,1.0f),
    };
    constexpr std::uint16_t kSkyIndices[] = {0, 1, 2, 0, 2, 3};
    constexpr PrimitiveVertex kArrowVertices[] = {
        make_vertex( 0.50f,  0.00f,  0.00f,  1,0,0, 0.5f,0.0f, 1.0f,0.92f,0.70f),
        make_vertex(-0.50f, -0.45f, -0.45f, -1,0,0, 0.0f,1.0f, 1.0f,0.92f,0.70f),
        make_vertex(-0.50f,  0.45f, -0.45f, -1,0,0, 1.0f,1.0f, 1.0f,0.92f,0.70f),
        make_vertex(-0.50f,  0.45f,  0.45f, -1,0,0, 1.0f,0.0f, 1.0f,0.92f,0.70f),
        make_vertex(-0.50f, -0.45f,  0.45f, -1,0,0, 0.0f,0.0f, 1.0f,0.92f,0.70f),
    };
    constexpr std::uint16_t kArrowIndices[] = {
        0, 1, 2,
        0, 2, 3,
        0, 3, 4,
        0, 4, 1,
        1, 4, 3,
        1, 3, 2,
    };

    if (!upload_mesh(dev, out.cube, kCubeVertices, std::size(kCubeVertices),
                     kCubeIndices, std::size(kCubeIndices), "arcade_cube_mesh") ||
        !upload_mesh(dev, out.arrow, kArrowVertices, std::size(kArrowVertices),
                     kArrowIndices, std::size(kArrowIndices), "arcade_gizmo_arrow_mesh") ||
        !upload_mesh(dev, out.plane, kPlaneVertices, std::size(kPlaneVertices),
                     kPlaneIndices, std::size(kPlaneIndices), "arcade_plane_mesh") ||
        !upload_mesh(dev, out.grid, kGridVertices, std::size(kGridVertices),
                     kGridIndices, std::size(kGridIndices), "arcade_editor_grid_mesh") ||
        !upload_mesh(dev, out.sky, kSkyVertices, std::size(kSkyVertices),
                     kSkyIndices, std::size(kSkyIndices), "arcade_sky_mesh")) {
        return false;
    }

    const std::vector<PrimitiveVertex> sphere_vertices = build_sphere_vertices(12, 24);
    const std::vector<std::uint16_t> sphere_indices = build_sphere_indices(12, 24);
    if (!upload_mesh(dev, out.sphere,
                     sphere_vertices.data(), sphere_vertices.size(),
                     sphere_indices.data(), sphere_indices.size(),
                     "arcade_sphere_mesh")) {
        return false;
    }

    // Humanoid agent capsule: radius 0.4 m, total height 1.8 m (cylinder 1.0 m
    // + two 0.4 m hemispheres). Fixed size — all mass agents share it.
    const std::vector<PrimitiveVertex> capsule_vertices =
        build_capsule_vertices(12, 24, 0.4f, 1.0f);
    if (!upload_mesh(dev, out.capsule,
                     capsule_vertices.data(), capsule_vertices.size(),
                     sphere_indices.data(), sphere_indices.size(),
                     "arcade_capsule_mesh")) {
        return false;
    }

    auto make_pipeline_desc = [](bool depth_test, bool depth_write,
                                 sh::FillMode fill = sh::FillMode::Solid) {
        sh::GraphicsPipelineDesc gp{};
#if defined(PSYNDER_GX_PRIMITIVE_SHADER_PATH)
        gp.slang_path = PSYNDER_GX_PRIMITIVE_SHADER_PATH;
#else
        gp.slang_path = "samples/02_crate/shaders/primitive_unlit.slang";
#endif
        gp.entry_point_vs = "vs_main";
        gp.entry_point_fs = "fs_main";
        gp.color_format_count = 1;
        gp.color_formats[0] = static_cast<std::uint8_t>(g::Format::Bgra8Srgb);
        gp.depth_format = depth_test
            ? static_cast<std::uint8_t>(g::Format::Depth32Float)
            : 0u;
        gp.enable_depth_write = depth_write;
        gp.enable_blend = true;
        gp.fill_mode = fill;
        sh::VertexInputDesc& vi = gp.vertex_input;
        vi.binding_count = 1;
        vi.bindings[0].stride = static_cast<std::uint16_t>(sizeof(PrimitiveVertex));
        vi.bindings[0].input_rate = sh::VertexInputRate::Vertex;
        vi.attr_count = 4;
        vi.attrs[0] = {sh::VertexAttrSemantic::Position, sh::VertexAttrFormat::Float32x3, 0, 0};
        vi.attrs[1] = {sh::VertexAttrSemantic::Normal, sh::VertexAttrFormat::Float32x3, 0, 12};
        vi.attrs[2] = {sh::VertexAttrSemantic::TexCoord0, sh::VertexAttrFormat::Float32x2, 0, 24};
        vi.attrs[3] = {sh::VertexAttrSemantic::Color0, sh::VertexAttrFormat::Float32x4, 0, 32};
        return gp;
    };
    out.scene_pipeline = sh::create_graphics(make_pipeline_desc(true, true));
    out.overlay_pipeline = sh::create_graphics(make_pipeline_desc(false, false));
    out.overlay_depth_pipeline = sh::create_graphics(make_pipeline_desc(true, false));
    // Translucent wireframe outlines for collider debug draws: depth-test
    // on (so outlines occlude correctly), depth-write off (overlay), blend
    // on (keep the existing translucent alpha), FillMode::Wireframe.
    out.wireframe_pipeline = sh::create_graphics(
        make_pipeline_desc(true, false, sh::FillMode::Wireframe));
    out.ready = out.scene_pipeline.valid() &&
        out.overlay_pipeline.valid() &&
        out.overlay_depth_pipeline.valid() &&
        out.wireframe_pipeline.valid();
    if (!out.ready && !quiet) {
        std::fputs("[PsyArcadeGX] primitive render pipeline unavailable\n", stderr);
    }
    return out.ready;
}

void shutdown_primitive_renderer(PrimitiveRenderResources& r) {
    if (r.scene_pipeline.valid()) {
        psynder::shader::destroy_pipeline(r.scene_pipeline);
        r.scene_pipeline = {};
    }
    if (r.overlay_pipeline.valid()) {
        psynder::shader::destroy_pipeline(r.overlay_pipeline);
        r.overlay_pipeline = {};
    }
    if (r.wireframe_pipeline.valid()) {
        psynder::shader::destroy_pipeline(r.wireframe_pipeline);
        r.wireframe_pipeline = {};
    }
    if (r.overlay_depth_pipeline.valid()) {
        psynder::shader::destroy_pipeline(r.overlay_depth_pipeline);
        r.overlay_depth_pipeline = {};
    }
    r.cube = {};
    r.arrow = {};
    r.sphere = {};
    r.capsule = {};
    r.plane = {};
    r.grid = {};
    r.sky = {};
    r.scene_depth = {};
    r.scene_depth_width = 0;
    r.scene_depth_height = 0;
    r.ready = false;
}

const PrimitiveMeshGpu& mesh_for_primitive(const PrimitiveRenderResources& resources,
                                           ScenePrimitiveKind kind) noexcept {
    switch (kind) {
        case ScenePrimitiveKind::Sphere: return resources.sphere;
        case ScenePrimitiveKind::Plane:  return resources.plane;
        case ScenePrimitiveKind::Cube:   return resources.cube;
    }
    return resources.cube;
}

const PrimitiveMeshGpu& mesh_for_collider(const PrimitiveRenderResources& resources,
                                          psynder::scene::ShapeKind kind) noexcept {
    switch (kind) {
        case psynder::scene::ShapeKind::Sphere:  return resources.sphere;
        case psynder::scene::ShapeKind::Capsule: return resources.capsule;
        case psynder::scene::ShapeKind::Plane:   return resources.plane;
        case psynder::scene::ShapeKind::Box:     return resources.cube;
    }
    return resources.cube;
}

bool ensure_scene_depth_target(psynder::gpu::Device* dev,
                               PrimitiveRenderResources& resources,
                               std::uint32_t width,
                               std::uint32_t height,
                               bool quiet) {
    namespace g = psynder::gpu;
    if (!dev || width == 0 || height == 0) {
        return false;
    }
    if (resources.scene_depth &&
        resources.scene_depth_width == width &&
        resources.scene_depth_height == height) {
        return true;
    }
    g::TextureDesc td{};
    td.width = width;
    td.height = height;
    td.depth = 1;
    td.mips = 1;
    td.array_layers = 1;
    td.format = g::Format::Depth32Float;
    td.usage = static_cast<std::uint32_t>(g::TextureUsage::DepthStencil);
    td.heap = g::HeapKind::DeviceLocal;
    td.debug_name = "arcade_scene_depth";
    resources.scene_depth = g::create_texture(dev, td);
    resources.scene_depth_width = resources.scene_depth ? width : 0;
    resources.scene_depth_height = resources.scene_depth ? height : 0;
    if (!resources.scene_depth && !quiet) {
        std::fputs("[PsyArcadeGX] scene depth target unavailable\n", stderr);
    }
    return static_cast<bool>(resources.scene_depth);
}

void set_identity_mvp(ScenePushConstants& pc) noexcept {
    for (float& value : pc.mvp) {
        value = 0.0f;
    }
    pc.mvp[0] = 1.0f;
    pc.mvp[5] = 1.0f;
    pc.mvp[10] = 1.0f;
    pc.mvp[15] = 1.0f;
}

psynder::math::Vec3 scene_sun_to_light_dir(const SceneEnvironment& environment) noexcept {
    namespace m = psynder::math;
    return m::normalize(m::mul(environment.sun_direction, -1.0f));
}

float scene_sun_strength(const SceneEnvironment& environment) noexcept {
    return std::clamp(environment.sun_intensity_lux / 100000.0f, 0.0f, 1.4f);
}

ScenePushConstants compose_sky_push(const SceneEnvironment& environment,
                                    float time_seconds) noexcept {
    ScenePushConstants pc{};
    set_identity_mvp(pc);
    pc.material[0] = environment.sun_color.x;
    pc.material[1] = environment.sun_color.y;
    pc.material[2] = environment.sun_color.z;
    pc.material[3] = environment.time_of_day_hours / 24.0f;
    pc.params[0] = environment.cloud_coverage;
    pc.params[1] = environment.cloud_density;
    pc.params[2] = time_seconds * environment.cloud_speed;
    pc.params[3] = -1.0f;
    pc.light[0] = environment.sun_direction.x;
    pc.light[1] = environment.sun_direction.y;
    pc.light[2] = environment.sun_direction.z;
    pc.light[3] = scene_sun_strength(environment);
    pc.ambient_exposure[0] = environment.ambient_color.x * environment.ambient_intensity;
    pc.ambient_exposure[1] = environment.ambient_color.y * environment.ambient_intensity;
    pc.ambient_exposure[2] = environment.ambient_color.z * environment.ambient_intensity;
    pc.ambient_exposure[3] = environment.exposure;
    return pc;
}

ScenePushConstants compose_scene_push(const ScenePrimitive& primitive,
                                      const SceneCamera& camera,
                                      const SceneEnvironment& environment,
                                      RenderDebugMode debug_mode,
                                      float time_seconds,
                                      float aspect) noexcept {
    namespace m = psynder::math;
    (void)time_seconds;
    const m::Quat rot_x = m::quat_from_axis_angle(
        {1.0f, 0.0f, 0.0f},
        primitive.rotation_euler_deg.x * m::kDegToRad);
    const m::Quat rot_y = m::quat_from_axis_angle(
        {0.0f, 1.0f, 0.0f},
        primitive.rotation_euler_deg.y * m::kDegToRad);
    const m::Quat rot_z = m::quat_from_axis_angle(
        {0.0f, 0.0f, 1.0f},
        primitive.rotation_euler_deg.z * m::kDegToRad);
    const m::Quat rotation = m::quat_normalize(m::quat_mul(rot_z, m::quat_mul(rot_y, rot_x)));
    const m::Mat4 model = m::mul(
        m::translate(primitive.position),
        m::mul(m::rotate_quat(rotation), m::scale(primitive.scale)));
    const m::Vec3 pitch_yaw{
        camera.rotation_euler_deg.x * m::kDegToRad,
        camera.rotation_euler_deg.y * m::kDegToRad,
        camera.rotation_euler_deg.z * m::kDegToRad,
    };
    const m::Vec3 forward{
        std::cos(pitch_yaw.x) * std::sin(pitch_yaw.y),
        -std::sin(pitch_yaw.x),
        std::cos(pitch_yaw.x) * std::cos(pitch_yaw.y),
    };
    const m::Vec3 target{
        camera.position.x + forward.x,
        camera.position.y + forward.y,
        camera.position.z + forward.z,
    };
    const m::Mat4 view = m::look_at_rh(camera.position, target, {0.0f, 1.0f, 0.0f});
    const m::Mat4 proj = m::perspective_rh(
        camera.fov_y_deg * m::kDegToRad,
        aspect > 0.0f ? aspect : 16.0f / 9.0f,
        0.05f,
        200.0f);

    ScenePushConstants pc{};
    const m::Mat4 mvp = m::mul(proj, m::mul(view, model));
    std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
    if (debug_mode == RenderDebugMode::Depth) {
        const float distance = m::length(m::sub(primitive.position, camera.position));
        const float z = std::clamp(distance / 32.0f, 0.0f, 1.0f);
        pc.material[0] = 1.0f - z;
        pc.material[1] = 0.25f + z * 0.55f;
        pc.material[2] = 0.55f + z * 0.65f;
    } else {
        pc.material[0] = primitive.material.albedo.x;
        pc.material[1] = primitive.material.albedo.y;
        pc.material[2] = primitive.material.albedo.z;
    }
    pc.material[0] += primitive.material.emissive.x * primitive.material.emissive_intensity;
    pc.material[1] += primitive.material.emissive.y * primitive.material.emissive_intensity;
    pc.material[2] += primitive.material.emissive.z * primitive.material.emissive_intensity;
    pc.material[3] = 1.0f;
    pc.params[0] = primitive.material.roughness;
    pc.params[1] = primitive.material.metallic;
    pc.params[2] = primitive.selected ? 1.0f : 0.0f;
    pc.params[3] = 2.0f;
    const m::Vec3 world_light = scene_sun_to_light_dir(environment);
    const m::Vec3 local_light = m::normalize(
        rotate_vec_by_quat(
            quat_conjugate_local(rotation),
            world_light));
    pc.light[0] = local_light.x;
    pc.light[1] = local_light.y;
    pc.light[2] = local_light.z;
    pc.light[3] = debug_mode == RenderDebugMode::Depth ? 0.0f : scene_sun_strength(environment);
    pc.ambient_exposure[0] = environment.ambient_color.x * environment.ambient_intensity;
    pc.ambient_exposure[1] = environment.ambient_color.y * environment.ambient_intensity;
    pc.ambient_exposure[2] = environment.ambient_color.z * environment.ambient_intensity;
    pc.ambient_exposure[3] = environment.exposure;
    return pc;
}

// Build scene push constants for an ECS prop entity: the model matrix comes
// straight off TransformWS (it already bakes T*R*S), and the surface comes off
// the canonical RenderMaterial. The light is taken into the model's local
// space (the shader dots local-space normals against it) by projecting the
// world light onto the model's normalised basis columns.
ScenePushConstants compose_scene_push_ecs(const psynder::math::Mat4& model,
                                          const psynder::scene::RenderMaterial& material,
                                          const SceneCamera& camera,
                                          const SceneEnvironment& environment,
                                          RenderDebugMode debug_mode,
                                          float aspect,
                                          float alpha = 1.0f) noexcept {
    namespace m = psynder::math;
    const m::Vec3 pitch_yaw{
        camera.rotation_euler_deg.x * m::kDegToRad,
        camera.rotation_euler_deg.y * m::kDegToRad,
        camera.rotation_euler_deg.z * m::kDegToRad,
    };
    const m::Vec3 forward{
        std::cos(pitch_yaw.x) * std::sin(pitch_yaw.y),
        -std::sin(pitch_yaw.x),
        std::cos(pitch_yaw.x) * std::cos(pitch_yaw.y),
    };
    const m::Vec3 target{
        camera.position.x + forward.x,
        camera.position.y + forward.y,
        camera.position.z + forward.z,
    };
    const m::Mat4 view = m::look_at_rh(camera.position, target, {0.0f, 1.0f, 0.0f});
    const m::Mat4 proj = m::perspective_rh(
        camera.fov_y_deg * m::kDegToRad,
        aspect > 0.0f ? aspect : 16.0f / 9.0f,
        0.05f,
        200.0f);

    ScenePushConstants pc{};
    const m::Mat4 mvp = m::mul(proj, m::mul(view, model));
    std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));

    const m::Vec3 world_pos{model.m[12], model.m[13], model.m[14]};
    if (debug_mode == RenderDebugMode::Depth) {
        const float distance = m::length(m::sub(world_pos, camera.position));
        const float z = std::clamp(distance / 32.0f, 0.0f, 1.0f);
        pc.material[0] = 1.0f - z;
        pc.material[1] = 0.25f + z * 0.55f;
        pc.material[2] = 0.55f + z * 0.65f;
    } else {
        pc.material[0] = material.albedo.x;
        pc.material[1] = material.albedo.y;
        pc.material[2] = material.albedo.z;
    }
    pc.material[0] += material.emissive.x * material.emissive_intensity;
    pc.material[1] += material.emissive.y * material.emissive_intensity;
    pc.material[2] += material.emissive.z * material.emissive_intensity;
    pc.material[3] = alpha;  // 1 = opaque; debug overlays pass < 1 for blending
    pc.params[0] = material.roughness;
    pc.params[1] = material.metallic;
    pc.params[2] = 0.0f;
    pc.params[3] = 2.0f;

    // Normalised model basis columns -> transpose maps world light into local.
    m::Vec3 c0{model.m[0], model.m[1], model.m[2]};
    m::Vec3 c1{model.m[4], model.m[5], model.m[6]};
    m::Vec3 c2{model.m[8], model.m[9], model.m[10]};
    c0 = m::normalize(c0);
    c1 = m::normalize(c1);
    c2 = m::normalize(c2);
    const m::Vec3 world_light = scene_sun_to_light_dir(environment);
    const m::Vec3 local_light = m::normalize(m::Vec3{
        m::dot(c0, world_light), m::dot(c1, world_light), m::dot(c2, world_light)});
    pc.light[0] = local_light.x;
    pc.light[1] = local_light.y;
    pc.light[2] = local_light.z;
    pc.light[3] = debug_mode == RenderDebugMode::Depth ? 0.0f : scene_sun_strength(environment);
    pc.ambient_exposure[0] = environment.ambient_color.x * environment.ambient_intensity;
    pc.ambient_exposure[1] = environment.ambient_color.y * environment.ambient_intensity;
    pc.ambient_exposure[2] = environment.ambient_color.z * environment.ambient_intensity;
    pc.ambient_exposure[3] = environment.exposure;
    return pc;
}

ScenePushConstants compose_grid_push(const SceneCamera& camera, float aspect) noexcept {
    namespace m = psynder::math;
    const m::Mat4 model = m::mul(
        m::translate({0.0f, -0.002f, 0.0f}),
        m::scale({24.0f, 1.0f, 24.0f}));
    const m::Vec3 pitch_yaw{
        camera.rotation_euler_deg.x * m::kDegToRad,
        camera.rotation_euler_deg.y * m::kDegToRad,
        camera.rotation_euler_deg.z * m::kDegToRad,
    };
    const m::Vec3 forward{
        std::cos(pitch_yaw.x) * std::sin(pitch_yaw.y),
        -std::sin(pitch_yaw.x),
        std::cos(pitch_yaw.x) * std::cos(pitch_yaw.y),
    };
    const m::Vec3 target{
        camera.position.x + forward.x,
        camera.position.y + forward.y,
        camera.position.z + forward.z,
    };
    const m::Mat4 view = m::look_at_rh(camera.position, target, {0.0f, 1.0f, 0.0f});
    const m::Mat4 proj = m::perspective_rh(
        camera.fov_y_deg * m::kDegToRad,
        aspect > 0.0f ? aspect : 16.0f / 9.0f,
        0.05f,
        200.0f);

    ScenePushConstants pc{};
    const m::Mat4 mvp = m::mul(proj, m::mul(view, model));
    std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
    pc.material[0] = 0.58f;
    pc.material[1] = 0.86f;
    pc.material[2] = 1.0f;
    pc.material[3] = 0.02f;
    pc.params[0] = 48.0f;
    pc.params[1] = 0.012f;
    pc.params[2] = 0.0f;
    pc.params[3] = 0.0f;
    pc.light[2] = 1.0f;
    pc.ambient_exposure[3] = 1.0f;
    return pc;
}

ScenePushConstants compose_gizmo_box_push(const GizmoBox& box,
                                          const SceneCamera& camera,
                                          float aspect) noexcept {
    namespace m = psynder::math;
    const m::Quat rotation = gizmo_box_render_rotation(box);
    const m::Mat4 model = m::mul(
        m::translate(box.center),
        m::mul(m::rotate_quat(rotation), m::scale(m::mul(box.render_half_extent, 2.0f))));
    const m::Vec3 forward = camera_forward(camera);
    const m::Vec3 target{
        camera.position.x + forward.x,
        camera.position.y + forward.y,
        camera.position.z + forward.z,
    };
    const m::Mat4 view = m::look_at_rh(camera.position, target, {0.0f, 1.0f, 0.0f});
    const m::Mat4 proj = m::perspective_rh(
        camera.fov_y_deg * m::kDegToRad,
        aspect > 0.0f ? aspect : 16.0f / 9.0f,
        0.05f,
        200.0f);

    ScenePushConstants pc{};
    const m::Mat4 mvp = m::mul(proj, m::mul(view, model));
    std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
    pc.material[0] = box.tint[0];
    pc.material[1] = box.tint[1];
    pc.material[2] = box.tint[2];
    pc.material[3] = box.tint[3];
    pc.params[0] = 1.0f;
    pc.params[1] = 0.001f;
    pc.params[2] = 1.0f;
    pc.params[3] = 1.0f;
    pc.light[2] = 1.0f;
    pc.ambient_exposure[3] = 1.0f;
    return pc;
}

ScenePushConstants compose_player_start_marker_push(const ScenePlayerStart& marker,
                                                    const SceneCamera& camera,
                                                    float aspect) noexcept {
    namespace m = psynder::math;
    const m::Quat yaw =
        m::quat_from_axis_angle({0.0f, 1.0f, 0.0f},
                                marker.rotation_euler_deg.y * m::kDegToRad);
    const m::Vec3 center = m::add(marker.position, {0.0f, 0.42f, 0.0f});
    const float distance = m::length(m::sub(camera.position, center));
    const float size = std::clamp(distance * 0.035f, 0.22f, 0.70f);
    const m::Mat4 model = m::mul(
        m::translate(center),
        m::mul(m::rotate_quat(yaw), m::scale({size * 0.55f, size * 1.65f, size * 0.55f})));
    const m::Vec3 forward = camera_forward(camera);
    const m::Vec3 target{
        camera.position.x + forward.x,
        camera.position.y + forward.y,
        camera.position.z + forward.z,
    };
    const m::Mat4 view = m::look_at_rh(camera.position, target, {0.0f, 1.0f, 0.0f});
    const m::Mat4 proj = m::perspective_rh(
        camera.fov_y_deg * m::kDegToRad,
        aspect > 0.0f ? aspect : 16.0f / 9.0f,
        0.05f,
        200.0f);

    ScenePushConstants pc{};
    const m::Mat4 mvp = m::mul(proj, m::mul(view, model));
    std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
    pc.material[0] = marker.selected ? 0.25f : 0.18f;
    pc.material[1] = marker.selected ? 1.00f : 0.72f;
    pc.material[2] = marker.selected ? 0.95f : 1.00f;
    pc.material[3] = marker.selected ? 0.88f : 0.58f;
    pc.params[0] = 1.0f;
    pc.params[1] = 0.001f;
    pc.params[2] = marker.selected ? 1.0f : 0.0f;
    pc.params[3] = 1.0f;
    pc.light[2] = 1.0f;
    pc.ambient_exposure[3] = 1.0f;
    return pc;
}

ScenePushConstants compose_play_collision_marker_push(const PlayModeState& play,
                                                      const SceneCamera& camera,
                                                      float aspect) noexcept {
    namespace m = psynder::math;
    const m::Vec3 center = {
        play.eye_position.x,
        play.eye_position.y - play.eye_height_m + play.capsule_height_m * 0.5f,
        play.eye_position.z,
    };
    const m::Mat4 model = m::mul(
        m::translate(center),
        m::scale({play.capsule_radius_m * 2.0f,
                  play.capsule_height_m,
                  play.capsule_radius_m * 2.0f}));
    const m::Vec3 forward = camera_forward(camera);
    const m::Vec3 target{
        camera.position.x + forward.x,
        camera.position.y + forward.y,
        camera.position.z + forward.z,
    };
    const m::Mat4 view = m::look_at_rh(camera.position, target, {0.0f, 1.0f, 0.0f});
    const m::Mat4 proj = m::perspective_rh(
        camera.fov_y_deg * m::kDegToRad,
        aspect > 0.0f ? aspect : 16.0f / 9.0f,
        0.05f,
        200.0f);

    ScenePushConstants pc{};
    const m::Mat4 mvp = m::mul(proj, m::mul(view, model));
    std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
    pc.material[0] = 1.00f;
    pc.material[1] = 0.78f;
    pc.material[2] = 0.20f;
    pc.material[3] = 0.40f;
    pc.params[0] = 1.0f;
    pc.params[1] = 0.001f;
    pc.params[2] = 1.0f;
    pc.params[3] = 1.0f;
    pc.light[2] = 1.0f;
    pc.ambient_exposure[3] = 1.0f;
    return pc;
}

bool encode_player_boot_pass(psynder::gpu::CmdBuffer* cmd,
                             psynder::ui::imm::RuntimeConsoleGpu& overlay,
                             const PlayerBootState& boot,
                             bool overlay_ok,
                             float time_seconds,
                             std::uint32_t drawable_width,
                             std::uint32_t drawable_height,
                             std::uint32_t point_width,
                             std::uint32_t point_height,
                             const psynder::ui::imm::RuntimeOverlayFrameDesc& overlay_input) {
    namespace g = psynder::gpu;
    g::RenderPassDesc pass{};
    pass.color_count = 1;
    pass.colors[0].target = nullptr;
    pass.colors[0].load = g::LoadOp::Clear;
    pass.colors[0].store = g::StoreOp::Store;
    const float pulse = 0.5f + 0.5f * std::sin(time_seconds * 0.55f);
    pass.colors[0].clear_rgba[0] = 0.025f + pulse * 0.018f;
    pass.colors[0].clear_rgba[1] = 0.035f + pulse * 0.014f;
    pass.colors[0].clear_rgba[2] = 0.060f + pulse * 0.025f;
    pass.colors[0].clear_rgba[3] = 1.0f;
    pass.depth.target = nullptr;
    pass.swapchain = true;

    g::begin_render(cmd, pass);
    g::set_viewport(cmd, g::Viewport{
        0.0f, 0.0f,
        static_cast<float>(drawable_width),
        static_cast<float>(drawable_height),
        0.0f, 1.0f});
    g::set_scissor(cmd, g::Scissor{
        0,
        0,
        drawable_width,
        drawable_height});
    if (overlay_ok) {
        psynder::ui::imm::draw_runtime_intro_background_gpu(
            cmd,
            overlay.overlay,
            psynder::ui::imm::RuntimeIntroBackgroundDesc{
                static_cast<float>(drawable_width),
                static_cast<float>(drawable_height),
                time_seconds,
                1.0f});
    }
    psynder::ui::imm::RuntimeBootOverlayDesc boot_overlay{};
    const psynder::ui::imm::RuntimeBootOverlayDesc* boot_overlay_ptr = nullptr;
    if (boot.render_state == PlayerBootRenderState::NoSceneLoaded) {
        boot_overlay.title = "NO SCENE LOADED";
        boot_overlay.subtitle = "Start PsyArcadeGX Editor and create or open a scene";
        boot_overlay.action = "open editor";
        boot_overlay.time_seconds = time_seconds;
        boot_overlay_ptr = &boot_overlay;
    } else if (boot.render_state == PlayerBootRenderState::NoCameraRendering) {
        boot_overlay.title = "NO CAMERA RENDERING";
        boot_overlay.subtitle = "Scene is loaded, but no active camera is present";
        boot_overlay.action = "add camera in editor";
        boot_overlay.time_seconds = time_seconds;
        boot_overlay_ptr = &boot_overlay;
    }
    bool open_editor_clicked = false;
    if (overlay_ok) {
        open_editor_clicked =
            psynder::ui::imm::draw_runtime_overlays_gpu(cmd,
                                                        overlay,
                                                        static_cast<float>(point_width),
                                                        static_cast<float>(point_height),
                                                        boot_overlay_ptr,
                                                        &overlay_input);
    }
    g::end_render(cmd);
    return open_editor_clicked;
}

bool encode_player_scene_pass(psynder::gpu::CmdBuffer* cmd,
                              psynder::gpu::Device* dev,
                              psynder::ui::imm::RuntimeConsoleGpu& overlay,
                              PrimitiveRenderResources& primitive_resources,
                              const PlayerBootState& boot,
                              PlayModeState& play,
                              const SceneCamera& render_camera,
                              GizmoAxis active_gizmo_axis,
                              GizmoAxis hover_gizmo_axis,
                              bool overlay_ok,
                              float time_seconds,
                              std::uint32_t drawable_width,
                              std::uint32_t drawable_height,
                              std::uint32_t point_width,
                              std::uint32_t point_height,
                              const psynder::ui::imm::RuntimeOverlayFrameDesc& overlay_input) {
    namespace g = psynder::gpu;
    const auto apply_view = [&]() {
        g::set_viewport(cmd, g::Viewport{
        0.0f, 0.0f,
        static_cast<float>(drawable_width),
        static_cast<float>(drawable_height),
        0.0f, 1.0f});
        g::set_scissor(cmd, g::Scissor{
        0,
        0,
        drawable_width,
        drawable_height});
    };

    const float aspect = drawable_height > 0
        ? static_cast<float>(drawable_width) / static_cast<float>(drawable_height)
        : 16.0f / 9.0f;
    const bool depth_ready =
        ensure_scene_depth_target(dev,
                                  primitive_resources,
                                  drawable_width,
                                  drawable_height,
                                  true);

    g::RenderPassDesc clear_pass{};
    clear_pass.color_count = 1;
    clear_pass.colors[0].target = nullptr;
    clear_pass.colors[0].load = g::LoadOp::Clear;
    clear_pass.colors[0].store = g::StoreOp::Store;
    clear_pass.colors[0].clear_rgba[0] = boot.clear_rgba[0];
    clear_pass.colors[0].clear_rgba[1] = boot.clear_rgba[1];
    clear_pass.colors[0].clear_rgba[2] = boot.clear_rgba[2];
    clear_pass.colors[0].clear_rgba[3] = boot.clear_rgba[3];
    clear_pass.swapchain = true;

    g::begin_render(cmd, clear_pass);
    apply_view();
    g::end_render(cmd);

    if (primitive_resources.ready &&
        primitive_resources.overlay_pipeline.valid() &&
        primitive_resources.sky.vertex_buffer &&
        primitive_resources.sky.index_buffer &&
        primitive_resources.sky.index_count > 0) {
        g::RenderPassDesc sky_pass{};
        sky_pass.color_count = 1;
        sky_pass.colors[0].target = nullptr;
        sky_pass.colors[0].load = g::LoadOp::Load;
        sky_pass.colors[0].store = g::StoreOp::Store;
        sky_pass.swapchain = true;
        g::begin_render(cmd, sky_pass);
        apply_view();
        g::bind_pipeline(cmd, primitive_resources.overlay_pipeline);
        const ScenePushConstants pc = compose_sky_push(boot.environment, time_seconds);
        g::push_constants(cmd,
                          &pc,
                          static_cast<std::uint32_t>(sizeof(pc)),
                          g::ShaderStage::AllGfx);
        g::bind_vertex_buffer(cmd, 0, primitive_resources.sky.vertex_buffer.get(), 0);
        g::bind_index_buffer(cmd, primitive_resources.sky.index_buffer.get(), g::IndexType::U16, 0);
        g::draw_indexed(cmd, primitive_resources.sky.index_count, 1, 0, 0, 0);
        g::end_render(cmd);
    }

    if (primitive_resources.ready && primitive_resources.scene_pipeline.valid()) {
        g::RenderPassDesc scene_pass{};
        scene_pass.color_count = 1;
        scene_pass.colors[0].target = nullptr;
        scene_pass.colors[0].load = g::LoadOp::Load;
        scene_pass.colors[0].store = g::StoreOp::Store;
        if (depth_ready) {
            scene_pass.depth.target = primitive_resources.scene_depth.get();
            scene_pass.depth.load = g::LoadOp::Clear;
            scene_pass.depth.store = g::StoreOp::Store;
            scene_pass.depth.clear_depth = 1.0f;
        }
        scene_pass.swapchain = true;
        g::begin_render(cmd, scene_pass);
        apply_view();
        g::bind_pipeline(cmd, primitive_resources.scene_pipeline);
        if (play.active && play.sim_world) {
            // Play mode: draw the scene from the ECS prop entities — the same
            // scene::World the character controller collides against. The
            // canonical TransformWS + Collider + RenderMaterial drive geometry,
            // model matrix, and surface. DOTS chunk walk, no per-entity virtual.
            namespace s = psynder::scene;
            play.sim_world->for_each_chunk<s::TransformWS, s::Collider, s::RenderMaterial>(
                [&](std::size_t n, s::TransformWS* xf, s::Collider* col,
                    s::RenderMaterial* mat) {
                    for (std::size_t i = 0; i < n; ++i) {
                        const PrimitiveMeshGpu& mesh =
                            mesh_for_collider(primitive_resources, col[i].kind);
                        if (!mesh.vertex_buffer || !mesh.index_buffer ||
                            mesh.index_count == 0) {
                            continue;
                        }
                        const ScenePushConstants pc =
                            compose_scene_push_ecs(xf[i].mtw,
                                                   mat[i],
                                                   render_camera,
                                                   boot.environment,
                                                   boot.render_debug_mode,
                                                   aspect);
                        g::push_constants(cmd,
                                          &pc,
                                          static_cast<std::uint32_t>(sizeof(pc)),
                                          g::ShaderStage::AllGfx);
                        g::bind_vertex_buffer(cmd, 0, mesh.vertex_buffer.get(), 0);
                        g::bind_index_buffer(cmd, mesh.index_buffer.get(),
                                             g::IndexType::U16, 0);
                        g::draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
                    }
                });

            // ── Debug-draw overlays (console `debugdraw colliders|flow`) ──────
            if (g_debug_draw_colliders || g_debug_draw_flow) {
                namespace m = psynder::math;
                // Depth-test but DON'T write depth, and blend: the debug shells
                // read as a translucent tint over the real mesh instead of an
                // opaque z-fighting shell. Colliders bind the WIREFRAME pipeline
                // below so their volumes render as translucent outlines; flow
                // markers keep the filled overlay pipeline.
                const auto bind_filled_dbg = [&]() {
                    g::bind_pipeline(cmd, primitive_resources.overlay_depth_pipeline);
                };
                const auto bind_wire_dbg = [&]() {
                    if (primitive_resources.wireframe_pipeline.valid()) {
                        g::bind_pipeline(cmd, primitive_resources.wireframe_pipeline);
                    } else {
                        bind_filled_dbg();
                    }
                };
                bind_filled_dbg();
                constexpr float kDbgAlpha = 0.35f;
                // Draw a primitive mesh under `model` with a debug material.
                const auto draw_dbg = [&](const PrimitiveMeshGpu& mesh,
                                          const m::Mat4& model,
                                          const s::RenderMaterial& dm) {
                    if (!mesh.vertex_buffer || !mesh.index_buffer ||
                        mesh.index_count == 0) {
                        return;
                    }
                    ScenePushConstants pc = compose_scene_push_ecs(
                        model, dm, render_camera, boot.environment,
                        boot.render_debug_mode, aspect, kDbgAlpha);
                    // Force the UNLIT shader mode (params.w <= 1.5): the lit path
                    // hardcodes alpha = 1, which would defeat the blend. Unlit
                    // honours material.a so the shell reads as a translucent tint.
                    pc.params[3] = 1.0f;
                    g::push_constants(cmd, &pc,
                                      static_cast<std::uint32_t>(sizeof(pc)),
                                      g::ShaderStage::AllGfx);
                    g::bind_vertex_buffer(cmd, 0, mesh.vertex_buffer.get(), 0);
                    g::bind_index_buffer(cmd, mesh.index_buffer.get(),
                                         g::IndexType::U16, 0);
                    g::draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
                };
                // Uniform-scale a TRS matrix about its own centre (scales the 3x3
                // basis, leaves the translation column).
                const auto scaled = [](const m::Mat4& src, float s) {
                    m::Mat4 r = src;
                    for (int c = 0; c < 3; ++c)
                        for (int row = 0; row < 3; ++row) r.m[c * 4 + row] *= s;
                    return r;
                };

                if (g_debug_draw_colliders) {
                    // A bright per-type wireframe shell around every collision
                    // volume: agent = cyan, dynamic body = orange, static = green.
                    bind_wire_dbg();
                    play.sim_world->for_each_chunk_with_entities<s::TransformWS,
                                                                 s::Collider>(
                        [&](std::size_t n, const psynder::Entity* ents,
                            s::TransformWS* xf, s::Collider* col) {
                            for (std::size_t i = 0; i < n; ++i) {
                                m::Vec3 c{0.10f, 0.85f, 0.25f};  // static green
                                if (play.sim_world->get<s::Agent>(ents[i])) {
                                    c = {0.10f, 0.85f, 0.95f};   // agent cyan
                                } else if (play.sim_world->get<s::DynamicBody>(
                                               ents[i])) {
                                    c = {0.95f, 0.55f, 0.10f};   // dynamic orange
                                }
                                const s::RenderMaterial dm{c, 0.5f, 0.0f, c, 1.3f};
                                draw_dbg(mesh_for_collider(primitive_resources,
                                                           col[i].kind),
                                         scaled(xf[i].mtw, 1.04f), dm);
                            }
                        });
                }

                if (g_debug_draw_flow) {
                    // Per agent: a hot heading marker offset along its velocity,
                    // plus a yellow marker at its steering target. Flow markers
                    // stay filled (rebind in case colliders left wireframe bound).
                    bind_filled_dbg();
                    play.sim_world->for_each_chunk<s::TransformWS, s::AgentVelocity,
                                                   s::AgentTarget>(
                        [&](std::size_t n, s::TransformWS* xf,
                            s::AgentVelocity* vel, s::AgentTarget* tgt) {
                            for (std::size_t i = 0; i < n; ++i) {
                                const m::Vec3 p{xf[i].mtw.m[12], xf[i].mtw.m[13],
                                                xf[i].mtw.m[14]};
                                const m::Vec3 v = vel[i].velocity;
                                const float sp = m::length(v);
                                if (sp > 1e-3f) {
                                    const m::Vec3 np = m::add(
                                        p, m::mul(m::mul(v, 1.0f / sp), 0.6f));
                                    const m::Vec3 hot{
                                        1.0f, std::min(0.2f + 0.2f * sp, 0.9f), 0.05f};
                                    const s::RenderMaterial dm{hot, 0.4f, 0.0f, hot,
                                                               1.6f};
                                    draw_dbg(primitive_resources.cube,
                                             scaled(m::translate(np), 0.16f), dm);
                                }
                                const s::RenderMaterial tm{{0.95f, 0.9f, 0.2f}, 0.4f,
                                                           0.0f, {0.5f, 0.45f, 0.1f},
                                                           1.0f};
                                draw_dbg(primitive_resources.cube,
                                         scaled(m::translate(tgt[i].goal), 0.2f), tm);
                            }
                        });
                }
            }
        } else {
            for (const ScenePrimitive& primitive : boot.primitives) {
                const PrimitiveMeshGpu& mesh = mesh_for_primitive(primitive_resources, primitive.kind);
                if (!mesh.vertex_buffer || !mesh.index_buffer || mesh.index_count == 0) {
                    continue;
                }
                const ScenePushConstants pc =
                    compose_scene_push(primitive,
                                       render_camera,
                                       boot.environment,
                                       boot.render_debug_mode,
                                       time_seconds,
                                       aspect);
                g::push_constants(cmd,
                                  &pc,
                                  static_cast<std::uint32_t>(sizeof(pc)),
                                  g::ShaderStage::AllGfx);
                g::bind_vertex_buffer(cmd, 0, mesh.vertex_buffer.get(), 0);
                g::bind_index_buffer(cmd, mesh.index_buffer.get(), g::IndexType::U16, 0);
                g::draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
            }
        }
        g::end_render(cmd);
    }

    g::RenderPassDesc overlay_pass{};
    overlay_pass.color_count = 1;
    overlay_pass.colors[0].target = nullptr;
    overlay_pass.colors[0].load = g::LoadOp::Load;
    overlay_pass.colors[0].store = g::StoreOp::Store;
    if (depth_ready) {
        overlay_pass.depth.target = primitive_resources.scene_depth.get();
        overlay_pass.depth.load = g::LoadOp::Load;
        overlay_pass.depth.store = g::StoreOp::DontCare;
    }
    overlay_pass.swapchain = true;
    g::begin_render(cmd, overlay_pass);
    apply_view();
    if (primitive_resources.ready && primitive_resources.overlay_pipeline.valid()) {
        const auto bind_depth_overlay = [&]() {
            if (depth_ready && primitive_resources.overlay_depth_pipeline.valid()) {
                g::bind_pipeline(cmd, primitive_resources.overlay_depth_pipeline);
            } else {
                g::bind_pipeline(cmd, primitive_resources.overlay_pipeline);
            }
        };
        const auto bind_top_overlay = [&]() {
            g::bind_pipeline(cmd, primitive_resources.overlay_pipeline);
        };
        bind_depth_overlay();
        if (!play.active &&
            boot.editor_grid_visible &&
            primitive_resources.grid.vertex_buffer &&
            primitive_resources.grid.index_buffer &&
            primitive_resources.grid.index_count > 0) {
            const ScenePushConstants pc = compose_grid_push(render_camera, aspect);
            g::push_constants(cmd,
                              &pc,
                              static_cast<std::uint32_t>(sizeof(pc)),
                              g::ShaderStage::AllGfx);
            g::bind_vertex_buffer(cmd, 0, primitive_resources.grid.vertex_buffer.get(), 0);
            g::bind_index_buffer(cmd, primitive_resources.grid.index_buffer.get(), g::IndexType::U16, 0);
            g::draw_indexed(cmd, primitive_resources.grid.index_count, 1, 0, 0, 0);
        }
        if (primitive_resources.cube.vertex_buffer &&
            primitive_resources.cube.index_buffer &&
            primitive_resources.cube.index_count > 0) {
            // Editor-only authoring markers — hidden while playing.
          if (!play.active) {
            for (const ScenePlayerStart& player_start : boot.player_starts) {
                if (player_start.name.empty()) {
                    continue;
                }
                const ScenePushConstants pc =
                    compose_player_start_marker_push(player_start, render_camera, aspect);
                g::push_constants(cmd,
                                  &pc,
                                  static_cast<std::uint32_t>(sizeof(pc)),
                                  g::ShaderStage::AllGfx);
                g::bind_vertex_buffer(cmd, 0, primitive_resources.cube.vertex_buffer.get(), 0);
                g::bind_index_buffer(cmd,
                                     primitive_resources.cube.index_buffer.get(),
                                     g::IndexType::U16,
                                     0);
                g::draw_indexed(cmd, primitive_resources.cube.index_count, 1, 0, 0, 0);
            }
            for (const ScenePlayerController& controller : boot.player_controllers) {
                ScenePlayerStart marker{};
                marker.name = controller.name;
                marker.position = controller.position;
                marker.rotation_euler_deg = controller.rotation_euler_deg;
                marker.selected = controller.selected;
                const ScenePushConstants pc =
                    compose_player_start_marker_push(marker, render_camera, aspect);
                g::push_constants(cmd,
                                  &pc,
                                  static_cast<std::uint32_t>(sizeof(pc)),
                                  g::ShaderStage::AllGfx);
                g::bind_vertex_buffer(cmd, 0, primitive_resources.cube.vertex_buffer.get(), 0);
                g::bind_index_buffer(cmd,
                                     primitive_resources.cube.index_buffer.get(),
                                     g::IndexType::U16,
                                     0);
                g::draw_indexed(cmd, primitive_resources.cube.index_count, 1, 0, 0, 0);
            }
          }  // !play.active (editor authoring markers)
            if (play.active && play.collision_debug) {
                const ScenePushConstants pc =
                    compose_play_collision_marker_push(play, render_camera, aspect);
                g::push_constants(cmd,
                                  &pc,
                                  static_cast<std::uint32_t>(sizeof(pc)),
                                  g::ShaderStage::AllGfx);
                g::bind_vertex_buffer(cmd, 0, primitive_resources.cube.vertex_buffer.get(), 0);
                g::bind_index_buffer(cmd,
                                     primitive_resources.cube.index_buffer.get(),
                                     g::IndexType::U16,
                                     0);
                g::draw_indexed(cmd, primitive_resources.cube.index_count, 1, 0, 0, 0);
            }
        }
        SelectedSceneTransform selected_transform{};
        if (!play.active &&
            selected_scene_transform(boot, selected_transform) &&
            !(boot.transform_mode == TransformMode::Scale && !selected_transform.allow_scale) &&
            primitive_resources.cube.vertex_buffer &&
            primitive_resources.cube.index_buffer &&
            primitive_resources.cube.index_count > 0 &&
            primitive_resources.arrow.vertex_buffer &&
            primitive_resources.arrow.index_buffer &&
            primitive_resources.arrow.index_count > 0) {
            GizmoBox boxes[kMaxGizmoBoxes]{};
            std::size_t box_count = 0;
            build_runtime_gizmo_boxes(boot.transform_mode,
                                      selected_transform.position,
                                      gizmo_world_scale(render_camera, selected_transform.position),
                                      active_gizmo_axis,
                                      hover_gizmo_axis,
                                      boxes,
                                      box_count);
            bind_top_overlay();
            for (std::size_t i = 0; i < box_count; ++i) {
                const PrimitiveMeshGpu& mesh =
                    boxes[i].arrow_head ? primitive_resources.arrow : primitive_resources.cube;
                if (!mesh.vertex_buffer || !mesh.index_buffer || mesh.index_count == 0) {
                    continue;
                }
                const ScenePushConstants pc =
                    compose_gizmo_box_push(boxes[i], render_camera, aspect);
                g::push_constants(cmd,
                                  &pc,
                                  static_cast<std::uint32_t>(sizeof(pc)),
                                  g::ShaderStage::AllGfx);
                g::bind_vertex_buffer(cmd, 0, mesh.vertex_buffer.get(), 0);
                g::bind_index_buffer(cmd, mesh.index_buffer.get(), g::IndexType::U16, 0);
                g::draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
            }
        }
    }

    if (overlay_ok) {
        (void)psynder::ui::imm::draw_runtime_overlays_gpu(cmd,
                                                          overlay,
                                                          static_cast<float>(point_width),
                                                          static_cast<float>(point_height),
                                                          nullptr,
                                                          &overlay_input);
    }
    g::end_render(cmd);
    return false;
}

} // namespace

#if defined(PSYNDER_GX_PLATFORM_MACOS)

int main(int argc, char** argv) {
    namespace plat = psynder::platform::macos;
    const Args args = parse_args(argc, argv);

    if (!args.quiet) {
        std::printf("[%s] platform=macOS backend=Metal smoke_frames=%d\n",
                    PSYNDER_GX_APP_LOG_PREFIX,
                    args.smoke_frames);
    }

    psynder::jobs::JobSystem::Get().start();
    PlayerBootState boot = boot_player_project(args, args.quiet);
    EditorCameraState editor_camera{};
    sync_editor_camera_to_boot(editor_camera, boot, true);
    PlayModeState play_mode{};

    auto cam = default_camera();

    plat::WindowDesc wdesc;
    wdesc.title = PSYNDER_GX_APP_WINDOW_TITLE;
    wdesc.window_width = 1280;
    wdesc.window_height = 720;
    plat::Window* win = plat::create_window(wdesc);
    if (!win) {
        std::fprintf(stderr, "[%s] failed to create window\n", PSYNDER_GX_APP_LOG_PREFIX);
        psynder::jobs::JobSystem::Get().stop();
        return 1;
    }

    psynder::gpu::DeviceDesc ddesc{};
    ddesc.native_window_handle = plat::native_layer(win);
    psynder::gpu::Device* dev = psynder::gpu::create_device(ddesc);
    if (!dev) {
        std::fprintf(stderr, "[%s] create_device failed\n", PSYNDER_GX_APP_LOG_PREFIX);
        plat::destroy_window(win);
        psynder::jobs::JobSystem::Get().stop();
        return 1;
    }

    std::uint64_t frame_idx = 0;
    bool mouse_left_prev = false;
    bool mouse_prev_valid = false;
    float mouse_prev_x = 0.0f;
    float mouse_prev_y = 0.0f;
    GizmoDragState gizmo_drag{};
    psynder::console::init_runtime_console();
    register_sample_editor_commands(boot, &play_mode, &editor_camera);
    psynder::console::set_runtime_console_engine_launch(argc, argv);
    psynder::editor::ipc::install_console_bridge();
    psynder::editor::ipc::set_scene_document_changed_callback(
        [&boot, &editor_camera, &play_mode](std::string_view path, std::string_view document_json) {
            if (play_mode.active) {
                stop_play_mode(play_mode);
            }
            apply_scene_document_to_boot(boot, path, document_json);
            sync_editor_camera_to_boot(editor_camera, boot, false);
        });
    psynder::editor::ipc::seed_scene_document(boot.startup_scene,
                                              "Startup",
                                              boot.scene_document_json);
    psynder::editor::ipc::ServerDesc editor_ipc_desc{};
    editor_ipc_desc.port = configured_editor_ipc_port();
    editor_ipc_desc.require_session_token = false;
    psynder::console::set_runtime_console_editor_ipc_port(editor_ipc_desc.port);
    bool editor_ipc_started =
        psynder::editor::ipc::Server::Get().start(editor_ipc_desc);
    const bool editor_ipc_retry_enabled =
        std::getenv("PSYNDER_GX_RESTARTED_FROM_WEB") != nullptr;
    auto next_editor_ipc_retry = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    if (!editor_ipc_started && !args.quiet) {
        std::fprintf(stderr, "[%s] editor IPC unavailable\n", PSYNDER_GX_APP_LOG_PREFIX);
    }
    psynder::console::set_runtime_console_clipboard_setter([](std::string_view text) {
        const std::string copy{text};
        plat::set_clipboard_text(copy.c_str());
    });
    psynder::console::set_runtime_console_gpu_info_provider([dev] {
        return gpu_info_text(dev, "Metal");
    });
    psynder::ui::imm::RuntimeConsoleGpu console_overlay{};
    const bool console_overlay_ok =
        psynder::ui::imm::init_runtime_console_gpu(dev, console_overlay);
    if (!console_overlay_ok && !args.quiet) {
        std::fprintf(stderr, "[%s] runtime console overlay disabled\n", PSYNDER_GX_APP_LOG_PREFIX);
    }
    PrimitiveRenderResources primitive_resources{};
    const bool primitive_renderer_ok =
        init_primitive_renderer(dev, primitive_resources, args.quiet);
    if (!primitive_renderer_ok && !args.quiet) {
        std::fprintf(stderr, "[%s] scene primitive renderer disabled\n", PSYNDER_GX_APP_LOG_PREFIX);
    }
    const bool script_started = psynder::script::Vm::Get().start();
    if (!script_started && !args.quiet) {
        std::fprintf(stderr, "[%s] script VM failed to start\n", PSYNDER_GX_APP_LOG_PREFIX);
    }
    auto& world = psynder::scene::World::Get();
    const psynder::Entity crate = create_script_crate(world, script_started);
    psynder::console::set_runtime_console_render_stats_provider(
        [&frame_idx, console_overlay_ok, &boot, &play_mode] {
            const std::string selected = selected_entity_runtime_label(boot);
            char buf[1024]{};
            std::snprintf(buf,
                          sizeof(buf),
                          "frames: %llu\npass: %s\n"
                          "overlay: %s\n"
                          "selected: %s\n"
                          "tool_mode: %s\n"
                          "render_debug: %s\n"
                          "scene_state: %s\n"
                          "play_state: %s\n"
                          "input_capture: %s\n"
                          "play_colliders: %u\n"
                          "collision_debug: %s\n"
                          "primitives: %zu\n"
                          "player_starts: %zu\n"
                          "player_controllers: %zu\n"
                          "camera: %s\n"
                          "sky: %s %.2fh clouds %.2f\n"
                          "vfs_mounts: %zu\n"
                          "%s%s",
                          static_cast<unsigned long long>(frame_idx),
                          PSYNDER_GX_APP_PASS_LABEL,
                          console_overlay_ok ? "enabled" : "disabled",
                          selected.c_str(),
                          transform_mode_token(boot.transform_mode).c_str(),
                          render_debug_token(boot.render_debug_mode).c_str(),
                          boot.render_state == PlayerBootRenderState::SceneRendering
                              ? "rendering"
                              : boot.render_state == PlayerBootRenderState::NoCameraRendering
                                    ? "no camera"
                                    : "no scene",
                          play_mode.active ? "playing" : "editing",
                          play_mode.input_captured ? "captured" : "released",
                          play_mode.static_collider_count,
                          play_mode.collision_debug ? "on" : "off",
                          static_cast<std::size_t>(boot.primitives.size()),
                          static_cast<std::size_t>(boot.player_starts.size()),
                          static_cast<std::size_t>(boot.player_controllers.size()),
                          boot.camera.active ? boot.camera.name.c_str() : "<none>",
                          boot.environment.sky_mode.c_str(),
                          static_cast<double>(boot.environment.time_of_day_hours),
                          static_cast<double>(boot.environment.cloud_coverage),
                          static_cast<std::size_t>(psynder::asset::Vfs::Get().mount_count()),
                          boot.manifest_status.c_str(),
                          boot.scene_status.c_str());
            return std::string{buf};
        });
    psynder::console::set_runtime_console_scene_stats_provider(
        [&world, crate, script_started] {
            char buf[192]{};
            std::snprintf(buf,
                          sizeof(buf),
                          "%s: 1\ncrate_alive: %s\n"
                          "script_spawn: %s\n",
                          PSYNDER_GX_APP_ENTITY_LABEL,
                          world.alive(crate) ? "yes" : "no",
                          script_started ? "attempted" : "fallback");
            return std::string{buf};
        });
    psynder::console::set_runtime_console_script_reload_provider(
        [script_started] {
            return script_started
                ? std::string{"script_reload: inline sample bootstrap has no file binding\n"}
                : std::string{"script_reload: script VM unavailable\n"};
        });

    using FrameClock = std::chrono::steady_clock;
    auto elapsed_ms = [](FrameClock::time_point begin,
                         FrameClock::time_point end) -> double {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    bool restart_on_exit = false;
    auto previous_frame_start = FrameClock::now();
    auto next_viewport_camera_sync = FrameClock::now();
    bool viewport_camera_dirty = false;
    bool mouse_captured_now = false;  // FPS cursor lock state (play mode)

    while (!plat::should_close(win)) {
        const auto frame_start = FrameClock::now();
        const float dt_seconds = std::clamp(
            std::chrono::duration<float>(frame_start - previous_frame_start).count(),
            0.0f,
            1.0f / 15.0f);
        previous_frame_start = frame_start;
        plat::pump_events();
        auto* input = psynder::platform::input();
        const bool toggle_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Tilde);
        const bool escape_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Escape);
        const bool enter_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Enter);
        const bool backspace_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Backspace);
        const bool history_prev_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Up);
        const bool history_next_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Down);
        const bool backspace_down =
            input && input->key_down(psynder::platform::KeyCode::Backspace);
        const bool history_prev_down =
            input && input->key_down(psynder::platform::KeyCode::Up);
        const bool history_next_down =
            input && input->key_down(psynder::platform::KeyCode::Down);
        const bool edit_left_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Left);
        const bool edit_right_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Right);
        const bool edit_left_down =
            input && input->key_down(psynder::platform::KeyCode::Left);
        const bool edit_right_down =
            input && input->key_down(psynder::platform::KeyCode::Right);
        const bool edit_home_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Home);
        const bool edit_end_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::End);
        const bool edit_delete_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Delete);
        const bool shift_down =
            input && (input->key_down(psynder::platform::KeyCode::LeftShift) ||
                      input->key_down(psynder::platform::KeyCode::RightShift));
        const bool tab_pressed =
            input && input->key_pressed(psynder::platform::KeyCode::Tab);
        const psynder::platform::MouseState mouse =
            input ? input->mouse() : psynder::platform::MouseState{};
        psynder::platform::MouseState nav_mouse = mouse;
        const bool raw_mouse_available = mouse.dx != 0.0f || mouse.dy != 0.0f;
        if (play_mode.active && play_mode.input_captured) {
            if (raw_mouse_available) {
                nav_mouse.dx = mouse.dx;
                nav_mouse.dy = mouse.dy;
            } else if (mouse_prev_valid) {
                nav_mouse.dx = mouse.x - mouse_prev_x;
                nav_mouse.dy = mouse.y - mouse_prev_y;
            } else {
                nav_mouse.dx = 0.0f;
                nav_mouse.dy = 0.0f;
            }
        } else if (mouse_prev_valid && (mouse.left || mouse.right || mouse.middle)) {
            nav_mouse.dx = mouse.x - mouse_prev_x;
            nav_mouse.dy = mouse.y - mouse_prev_y;
        } else {
            nav_mouse.dx = 0.0f;
            nav_mouse.dy = 0.0f;
        }
        const bool mouse_left_pressed = mouse.left && !mouse_left_prev;
        const bool mouse_left_released = !mouse.left && mouse_left_prev;
        const float console_scroll = mouse.wheel;
        char console_text[128]{};
        const std::size_t console_text_len =
            plat::text_input_utf8(console_text, sizeof(console_text));
        const auto console_shortcuts = plat::consume_console_shortcuts();
        psynder::console::update_runtime_console(
            psynder::console::RuntimeConsoleInput{
                toggle_pressed,
                escape_pressed,
                enter_pressed,
                backspace_pressed,
                history_prev_pressed,
                history_next_pressed,
                backspace_down,
                history_prev_down,
                history_next_down,
                edit_left_pressed,
                edit_right_pressed,
                edit_left_down,
                edit_right_down,
                edit_home_pressed,
                edit_end_pressed,
                edit_delete_pressed,
                shift_down,
                console_shortcuts.copy,
                console_shortcuts.cut,
                console_shortcuts.paste,
                console_shortcuts.select_all,
                tab_pressed,
                console_scroll,
                mouse.x,
                mouse.y,
                mouse.left,
                static_cast<float>(plat::point_width(win)),
                static_cast<float>(plat::point_height(win)),
                std::string_view{console_shortcuts.paste_text ?
                                     console_shortcuts.paste_text : ""},
                std::string_view{console_text, console_text_len}});
        psynder::console::pump_runtime_console();
        if (editor_ipc_started) {
            psynder::editor::ipc::Server::Get().pump();
        } else if (editor_ipc_retry_enabled &&
                   FrameClock::now() >= next_editor_ipc_retry) {
            editor_ipc_started =
                psynder::editor::ipc::Server::Get().start(editor_ipc_desc);
            next_editor_ipc_retry = FrameClock::now() + std::chrono::seconds(2);
            if (editor_ipc_started && !args.quiet) {
                std::fprintf(stderr,
                             "[%s] editor IPC recovered\n",
                             PSYNDER_GX_APP_LOG_PREFIX);
            }
        }
        if (psynder::console::consume_runtime_console_restart_requested()) {
            restart_on_exit = true;
            plat::request_close(win);
        } else if (psynder::console::consume_runtime_console_quit_requested()) {
            // The runtime console turns a console-closed Esc into a quit
            // request. In play mode Esc is in-game input (release the mouse,
            // then return to editor — handled below), so swallow the quit
            // while playing instead of closing the whole app.
            if (!play_mode.active) {
                plat::request_close(win);
            }
        }
        if (!play_mode.active && !psynder::console::runtime_console_open() && input) {
            if (input->key_pressed(psynder::platform::KeyCode::F1)) {
                (void)sync_editor_transform_mode(boot, TransformMode::Translate);
            } else if (input->key_pressed(psynder::platform::KeyCode::F2)) {
                (void)sync_editor_transform_mode(boot, TransformMode::Rotate);
            } else if (input->key_pressed(psynder::platform::KeyCode::F3)) {
                (void)sync_editor_transform_mode(boot, TransformMode::Scale);
            }
        }
        if (play_mode.active && !psynder::console::runtime_console_open() && input &&
            input->key_pressed(psynder::platform::KeyCode::Escape)) {
            if (play_mode.input_captured) {
                play_mode.input_captured = false;
            } else {
                stop_play_mode(play_mode);
            }
        }
        // FPS mouse lock: hide + lock the cursor while play mode owns input.
        // Released when the console is open (so the cursor can type/click) or
        // when capture is toggled off / play mode stops.
        const bool want_mouse_capture = play_mode.active &&
                                        play_mode.input_captured &&
                                        !psynder::console::runtime_console_open();
        if (want_mouse_capture != mouse_captured_now) {
            plat::set_mouse_captured(want_mouse_capture);
            mouse_captured_now = want_mouse_capture;
        }
        reconcile_runtime_selection(boot);
        if (gizmo_drag.active && !scene_entity_exists(boot, gizmo_drag.entity_name)) {
            gizmo_drag = {};
        }
        bool camera_changed_this_frame = false;
        if (play_mode.active && !psynder::console::runtime_console_open()) {
            camera_changed_this_frame =
                update_play_mode(play_mode, boot, input, nav_mouse, dt_seconds);
        } else if (!psynder::console::runtime_console_open()) {
            camera_changed_this_frame =
                update_editor_camera(editor_camera, boot, input, nav_mouse, dt_seconds);
        } else {
            editor_camera.orbiting_prev = false;
        }
        // Hitscan fire: left-click in captured play mode raycasts the ECS props
        // (combat slice over the canonical Collider) and destroys the crate it
        // hits — "shoot a crate, it disappears." The destroy runs here, outside
        // any for_each_chunk, so it's a safe structural change. The ground and
        // large structures are spared (Plane / oversized colliders). A replay
        // (#38) re-fires here too via replay_fire_request (no capture needed).
        if (play_mode.active && play_mode.sim_world &&
            !psynder::console::runtime_console_open() &&
            ((play_mode.input_captured && mouse_left_pressed) ||
             play_mode.replay_fire_request)) {
            namespace mt = psynder::math;
            const float fire_yaw = play_mode.yaw_deg * mt::kDegToRad;
            const float fire_pitch = play_mode.pitch_deg * mt::kDegToRad;
            const mt::Vec3 fire_dir{
                std::cos(fire_pitch) * std::sin(fire_yaw),
                -std::sin(fire_pitch),
                std::cos(fire_pitch) * std::cos(fire_yaw),
            };
            const psynder::combat::Ray shot{play_mode.eye_position, fire_dir};
            const psynder::combat::Hit hit =
                psynder::combat::raycast_nearest(*play_mode.sim_world, shot, 100.0f);
            if (hit.hit && hit.entity != play_mode.pawn) {
                // A steering-crowd agent (ADR-019 class 1)? It has no Jolt body
                // and isn't a fracturable crate — just despawn it cleanly and
                // drop it from the crowd list so the chase/avoid loop skips it.
                bool was_agent = false;
                if (play_mode.sim_world->get<psynder::scene::Agent>(hit.entity)) {
                    for (std::size_t i = 0; i < play_mode.crowd.size(); ++i) {
                        if (play_mode.crowd[i] != hit.entity) continue;
                        play_mode.crowd[i] = play_mode.crowd.back();
                        play_mode.crowd.pop_back();
                        break;
                    }
                    play_mode.sim_world->destroy(hit.entity);
                    was_agent = true;
                }
                if (was_agent) { play_mode.replay_fire_request = false; }
                else {
                // A dynamic rigid body? Knock it back with an impulse along the
                // shot (ADR-019 class 2) instead of destroying it.
                bool knocked = false;
                for (auto& dyn : play_mode.dynamic_body_map) {
                    if (dyn.first != hit.entity) continue;
                    constexpr float kImpulse = 36.0f;  // kg*m/s along the shot
                    character_spine::dynamic_body_apply_impulse(
                        play_mode.phys_world, dyn.second,
                        fire_dir.x * kImpulse,
                        fire_dir.y * kImpulse + 6.0f,  // a little pop upward
                        fire_dir.z * kImpulse);
                    knocked = true;
                    break;
                }
                const auto* col = play_mode.sim_world->get<psynder::scene::Collider>(hit.entity);
                const float max_he = col
                    ? std::max({col->half_extents.x, col->half_extents.y, col->half_extents.z})
                    : 0.0f;
                if (!knocked && col && col->kind != psynder::scene::ShapeKind::Plane &&
                    max_he < 3.0f) {
                    // Snapshot the crate's transform + look before destroying it so
                    // we can fracture it into dynamic shards (ADR-021/#45).
                    const auto* hit_xf =
                        play_mode.sim_world->get<psynder::scene::TransformWS>(hit.entity);
                    const auto* hit_mat =
                        play_mode.sim_world->get<psynder::scene::RenderMaterial>(hit.entity);
                    if (hit_xf) {
                        const psynder::scene::RenderMaterial mat =
                            hit_mat ? *hit_mat
                                    : psynder::scene::RenderMaterial{
                                          {0.76f, 0.82f, 0.88f}, 0.65f, 0.0f,
                                          {0.0f, 0.0f, 0.0f}, 0.0f};
                        fracture_static_crate(play_mode, *hit_xf, *col, mat, fire_dir,
                                              hit.entity.raw);
                    }
                    play_mode.sim_world->destroy(hit.entity);
                    // Drop the matching Jolt static body too, or its collider
                    // would linger as an invisible wall (the collision ghost).
                    for (std::size_t i = 0; i < play_mode.static_body_map.size(); ++i) {
                        if (play_mode.static_body_map[i].first != hit.entity) continue;
                        character_spine::remove_static_body(
                            play_mode.phys_world, play_mode.static_body_map[i].second);
                        play_mode.static_body_map[i] = play_mode.static_body_map.back();
                        play_mode.static_body_map.pop_back();
                        break;
                    }
                }
                }  // end else (non-agent hit)
            }
            play_mode.replay_fire_request = false;
        }

        viewport_camera_dirty =
            viewport_camera_dirty || (!play_mode.active && camera_changed_this_frame);
        if (viewport_camera_dirty && frame_start >= next_viewport_camera_sync) {
            if (sync_editor_viewport_camera(boot, editor_camera)) {
                viewport_camera_dirty = false;
            }
            next_viewport_camera_sync = frame_start + std::chrono::milliseconds(250);
        }
        const auto input_end = FrameClock::now();

        psynder::ui::imm::RuntimeOverlayFrameDesc overlay_input{};
        overlay_input.mouse_pos = {mouse.x, mouse.y};
        overlay_input.mouse_down = mouse.left;
        overlay_input.mouse_pressed = mouse_left_pressed;
        overlay_input.mouse_released = mouse_left_released;

        const float t = static_cast<float>(frame_idx) / 60.0f;
        const SceneCamera render_camera = play_mode.active
            ? play_mode_camera(play_mode, boot.camera)
            : (editor_camera.initialized
                   ? editor_camera_to_scene_camera(editor_camera, boot.camera)
                   : boot.camera);
        const bool editor_orbit_modifier =
            input && (input->key_down(psynder::platform::KeyCode::LeftAlt) ||
                      input->key_down(psynder::platform::KeyCode::RightAlt));
        const float viewport_point_width = static_cast<float>(plat::point_width(win));
        const float viewport_point_height = static_cast<float>(plat::point_height(win));
        bool gizmo_consumed_left_press = false;
        GizmoAxis hovered_gizmo_axis = GizmoAxis::None;
        if (!gizmo_drag.active &&
            !mouse.right &&
            !mouse.middle &&
            !editor_orbit_modifier &&
            !play_mode.active &&
            !psynder::console::runtime_console_open() &&
            boot.render_state == PlayerBootRenderState::SceneRendering) {
            hovered_gizmo_axis = pick_runtime_gizmo(
                boot,
                render_camera,
                boot.transform_mode,
                mouse.x,
                mouse.y,
                viewport_point_width,
                viewport_point_height);
        }
        if (gizmo_drag.active) {
            if (mouse.left &&
                !psynder::console::runtime_console_open() &&
                !play_mode.active &&
                boot.render_state == PlayerBootRenderState::SceneRendering) {
                (void)update_gizmo_drag(gizmo_drag,
                                        boot,
                                        render_camera,
                                        mouse.x,
                                        mouse.y,
                                        viewport_point_width,
                                        viewport_point_height);
                gizmo_consumed_left_press = true;
            } else if (!mouse_left_released) {
                gizmo_drag = {};
            }
        }
        if (mouse_left_pressed &&
            !gizmo_drag.active &&
            !mouse.right &&
            !mouse.middle &&
            !editor_orbit_modifier &&
            !play_mode.active &&
            !psynder::console::runtime_console_open() &&
            boot.render_state == PlayerBootRenderState::SceneRendering) {
            const GizmoAxis picked_axis = pick_runtime_gizmo(
                boot,
                render_camera,
                boot.transform_mode,
                mouse.x,
                mouse.y,
                viewport_point_width,
                viewport_point_height);
            if (picked_axis != GizmoAxis::None &&
                begin_gizmo_drag(gizmo_drag,
                                 boot,
                                 render_camera,
                                 boot.transform_mode,
                                 picked_axis,
                                 mouse.x,
                                 mouse.y,
                                 viewport_point_width,
                                 viewport_point_height)) {
                broadcast_scene_sync_reply(boot, "gizmo drag begin");
                gizmo_consumed_left_press = true;
            }
        }
        if (mouse_left_released && gizmo_drag.active) {
            broadcast_scene_sync_reply(boot, "gizmo drag commit");
            gizmo_drag = {};
        }
        if (mouse_left_pressed &&
            !gizmo_consumed_left_press &&
            !mouse.right &&
            !mouse.middle &&
            !editor_orbit_modifier &&
            !play_mode.active &&
            !psynder::console::runtime_console_open() &&
            boot.render_state == PlayerBootRenderState::SceneRendering) {
            const std::string picked = pick_scene_primitive(
                boot,
                render_camera,
                mouse.x,
                mouse.y,
                viewport_point_width,
                viewport_point_height);
            (void)sync_editor_selection(boot, picked);
        }
        if (auto* tr = world.get<psynder::scene::TransformWS>(crate)) {
            *tr = crate_transform(t);
        }
        (void)psynder::camera::view_proj_matrix(
            cam,
            static_cast<float>(plat::drawable_width(win)) /
                static_cast<float>(plat::drawable_height(win) ? plat::drawable_height(win) : 1));
        const auto update_end = FrameClock::now();

        const auto publish_profiler = [&](double render_ms) {
            if (!editor_ipc_started) return;
            const std::uint32_t primitive_draws =
                boot.render_state == PlayerBootRenderState::SceneRendering &&
                primitive_renderer_ok
                    ? static_cast<std::uint32_t>(boot.primitives.size())
                    : 0u;
            const std::uint32_t boot_draws =
                boot.render_state == PlayerBootRenderState::SceneRendering
                    ? 0u
                    : (console_overlay_ok ? 2u : 0u);
            const std::uint32_t overlay_draws = console_overlay_ok ? 1u : 0u;
            const std::uint32_t draw_calls = primitive_draws + boot_draws + overlay_draws;
            const std::uint32_t entities =
                static_cast<std::uint32_t>(boot.primitives.size()) +
                (boot.render_state == PlayerBootRenderState::SceneRendering ? 1u : 0u);
            const auto frame_end = FrameClock::now();
            const psynder::editor::ipc::ProfilerSectionSample sections[] = {
                {"input+ipc", elapsed_ms(frame_start, input_end)},
                {"scene", elapsed_ms(input_end, update_end)},
                {"render", render_ms},
            };
            psynder::editor::ipc::publish_profiler_frame(
                frame_idx,
                elapsed_ms(frame_start, frame_end),
                0.0,
                draw_calls,
                entities,
                std::span<const psynder::editor::ipc::ProfilerSectionSample>{sections});
        };

        auto finish_loop = [&]() {
            ++frame_idx;
            return args.smoke_frames > 0 &&
                   frame_idx >= static_cast<std::uint64_t>(args.smoke_frames);
        };

        if (!psynder::gpu::begin_frame(dev)) {
            psynder::gpu::resize_swapchain(dev,
                plat::drawable_width(win),
                plat::drawable_height(win));
            if (finish_loop()) {
                if (!args.quiet) {
                    std::printf("[%s] smoke complete: %llu frames\n",
                                PSYNDER_GX_APP_LOG_PREFIX,
                                static_cast<unsigned long long>(frame_idx));
                }
                break;
            }
            mouse_left_prev = mouse.left;
            mouse_prev_valid = true;
            mouse_prev_x = mouse.x;
            mouse_prev_y = mouse.y;
            continue;
        }
        const auto render_start = FrameClock::now();
        if (auto* cb = psynder::gpu::cmd_open(dev)) {
            const bool open_editor_clicked =
                boot.render_state == PlayerBootRenderState::SceneRendering
                    ? encode_player_scene_pass(cb,
                                               dev,
                                               console_overlay,
                                               primitive_resources,
                                               boot,
                                               play_mode,
                                               render_camera,
                                               gizmo_drag.active
                                                   ? gizmo_drag.axis
                                                   : GizmoAxis::None,
                                               hovered_gizmo_axis,
                                               console_overlay_ok,
                                               t,
                                               plat::drawable_width(win),
                                               plat::drawable_height(win),
                                               plat::point_width(win),
                                               plat::point_height(win),
                                               overlay_input)
                    : encode_player_boot_pass(cb,
                                              console_overlay,
                                              boot,
                                              console_overlay_ok,
                                              t,
                                              plat::drawable_width(win),
                                              plat::drawable_height(win),
                                              plat::point_width(win),
                                              plat::point_height(win),
                                              overlay_input);
            psynder::gpu::cmd_submit(dev, cb);
            if (open_editor_clicked) {
                psynder::console::submit_runtime_console_line("web_console open");
            }
        }
        psynder::gpu::end_frame(dev);
        psynder::gpu::resize_swapchain(dev,
            plat::drawable_width(win),
            plat::drawable_height(win));
        publish_profiler(elapsed_ms(render_start, FrameClock::now()));

        if (finish_loop()) {
            if (!args.quiet) {
                std::printf("[%s] smoke complete: %llu frames\n",
                            PSYNDER_GX_APP_LOG_PREFIX,
                            static_cast<unsigned long long>(frame_idx));
            }
            break;
        }
        mouse_left_prev = mouse.left;
        mouse_prev_valid = true;
        mouse_prev_x = mouse.x;
        mouse_prev_y = mouse.y;
    }

        if (viewport_camera_dirty && !play_mode.active) {
        (void)sync_editor_viewport_camera(boot, editor_camera);
    }
    stop_play_mode(play_mode);
    plat::set_mouse_captured(false);  // restore the cursor on exit
    psynder::ui::imm::shutdown_runtime_console_gpu(console_overlay);
    shutdown_primitive_renderer(primitive_resources);
    psynder::console::set_runtime_console_clipboard_setter({});
    psynder::console::set_runtime_console_gpu_info_provider({});
    psynder::console::set_runtime_console_render_stats_provider({});
    psynder::console::set_runtime_console_scene_stats_provider({});
    psynder::console::set_runtime_console_script_reload_provider({});
    psynder::editor::ipc::remove_console_bridge();
    if (editor_ipc_started) {
        psynder::editor::ipc::Server::Get().stop();
    }
    psynder::gpu::destroy_device(dev);
    plat::destroy_window(win);
    if (script_started) {
        psynder::script::Vm::Get().shutdown();
    }
    psynder::jobs::JobSystem::Get().stop();
    if (restart_on_exit) {
        (void)relaunch_current_process(argc, argv, args.quiet);
    }
    return 0;
}

#elif defined(PSYNDER_GX_PLATFORM_WIN32)

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    std::printf("[%s] platform=Windows smoke_frames=%d\n",
                PSYNDER_GX_APP_LOG_PREFIX,
                args.smoke_frames);
    std::printf("[%s] Windows visual validation is reserved for the PC pass.\n",
                PSYNDER_GX_APP_LOG_PREFIX);
    return 0;
}

#elif defined(PSYNDER_GX_PLATFORM_LINUX)

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    std::printf("[%s] platform=Linux smoke_frames=%d\n",
                PSYNDER_GX_APP_LOG_PREFIX,
                args.smoke_frames);
    std::printf("[%s] Linux visual validation follows the Wayland/XCB pass.\n",
                PSYNDER_GX_APP_LOG_PREFIX);
    return 0;
}

#else

int main() {
    std::printf("[%s] no supported platform selected\n", PSYNDER_GX_APP_LOG_PREFIX);
    return 1;
}

#endif
