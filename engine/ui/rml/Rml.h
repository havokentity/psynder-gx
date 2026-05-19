// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ui/rml/Rml.h
//
// Lane 21 — RmlUi binding public header CONTRACT.
//
// GX Wave-B change: `render()` no longer accepts a CPU `render::Framebuffer&`.
// It takes a `psy::gpu::CmdBuffer*` + `psy::gpu::Device*` + viewport dims.
// The device pointer is needed only for the lazy-initialisation path and the
// glyph atlas upload; it is safe to pass nullptr during the first frame (the
// submit will be a no-op until the device is available).
//
// render/Framebuffer.h is NOT included here.

#pragma once

#include "core/Types.h"
#include "gpu/PublicGpu.h"

#include <string_view>

namespace psynder::ui::rml {

bool initialize();
void shutdown();

bool load_document(std::string_view virtual_path, std::string_view name);
void show(std::string_view name);
void hide(std::string_view name);

// Render all visible documents into `cmd`.
// `dev` is required for first-time resource creation; pass nullptr to skip
// the frame silently (safe for headless unit tests).
void render(psynder::gpu::CmdBuffer* cmd,
            psynder::gpu::Device*    dev,
            f32 viewport_width,
            f32 viewport_height);

void update(f32 dt);

}  // namespace psynder::ui::rml
