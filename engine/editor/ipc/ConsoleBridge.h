// SPDX-License-Identifier: MIT
// Psynder-GX — editor IPC bridge for the live engine console.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace psynder::editor::ipc {

using SceneDocumentChangedFn =
    std::function<void(std::string_view path, std::string_view document_json)>;

struct ProfilerSectionSample {
    std::string_view name;
    double ms = 0.0;
};

// Installs the engine-thread message handler used by Server::pump().
// Safe to call repeatedly; the latest install replaces the previous handler.
void install_console_bridge();

// Optional live-scene hook for the running player/editor host. Called on the
// thread that invokes Server::pump() after scene create/load/save succeeds.
void set_scene_document_changed_callback(SceneDocumentChangedFn cb);

// Seeds the editor's scene-authoring state from the scene the player loaded
// during boot. This keeps web panels in sync before the user presses Load.
void seed_scene_document(std::string_view path,
                         std::string_view name,
                         std::string_view document_json);

void publish_profiler_frame(std::uint64_t frame_index,
                            double cpu_ms,
                            double gpu_ms,
                            std::uint32_t draw_calls,
                            std::uint32_t entities,
                            std::span<const ProfilerSectionSample> sections = {});

// Clears the bridge handler. Call before Server::stop() during shutdown.
void remove_console_bridge();

}  // namespace psynder::editor::ipc
