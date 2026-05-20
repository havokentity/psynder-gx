// SPDX-License-Identifier: MIT
// Psynder-GX — reflect the frozen GX render components (engine/scene/
// GxComponents.h) and a sample system into the script reflection registries.
//
// This is the "declare once" demonstration on real engine types: each
// PSYNDER_SCRIPT_COMPONENT below feeds both the engine ECS (via
// scene::component_id<T>()) and the Lua-visible `reflect` schema. Padding
// members (_padN) are intentionally omitted — they carry no semantics.
//
// Because psynder_script is a STATIC library, a TU consisting solely of
// static-init registrations could be dropped by the linker. anchor_gx_
// reflections() (referenced from install_reflect_api in Reflect.cpp) forces
// this TU to be linked and its initializers to run before the registries are
// first read.

#include "script/Reflect.h"

#include "scene/GxComponents.h"

namespace psynder::script::detail {

void anchor_gx_reflections() noexcept {
    // Intentionally empty — see the file header. The mere existence of this
    // odr-used, non-inline definition is what pulls the TU into the link.
}

}  // namespace psynder::script::detail

// Bring the GX component types into scope unqualified so the macros can both
// stringify (Lua name) and token-paste (storage symbol) each type.
using namespace ::psynder::scene;

namespace {

// Sample kinematic mover. A production system would integrate motion into
// TransformWS through World queries; here only the schedule contract matters
// (writes TransformWS, reads nothing), which the scheduler + REPL introspect.
void advance_movers(World& /*world*/, ::psynder::f64 /*dt*/) {}

}  // namespace

PSYNDER_SCRIPT_COMPONENT(TransformWS, PSYNDER_SCRIPT_FIELD(TransformWS, mtw),
                         PSYNDER_SCRIPT_FIELD(TransformWS, prev_mtw));

PSYNDER_SCRIPT_COMPONENT(MeshRef, PSYNDER_SCRIPT_FIELD(MeshRef, mesh),
                         PSYNDER_SCRIPT_FIELD(MeshRef, lod_bias));

PSYNDER_SCRIPT_COMPONENT(MaterialRef, PSYNDER_SCRIPT_FIELD(MaterialRef, material));

PSYNDER_SCRIPT_COMPONENT(LightPoint, PSYNDER_SCRIPT_FIELD(LightPoint, position),
                         PSYNDER_SCRIPT_FIELD(LightPoint, radius),
                         PSYNDER_SCRIPT_FIELD(LightPoint, color),
                         PSYNDER_SCRIPT_FIELD(LightPoint, intensity));

PSYNDER_SCRIPT_COMPONENT(LightDirectional,
                         PSYNDER_SCRIPT_FIELD(LightDirectional, direction),
                         PSYNDER_SCRIPT_FIELD(LightDirectional, intensity),
                         PSYNDER_SCRIPT_FIELD(LightDirectional, color),
                         PSYNDER_SCRIPT_FIELD(LightDirectional, cast_rt_shadow));

PSYNDER_SCRIPT_COMPONENT(CameraComponent,
                         PSYNDER_SCRIPT_FIELD(CameraComponent, view_mtx),
                         PSYNDER_SCRIPT_FIELD(CameraComponent, proj_mtx),
                         PSYNDER_SCRIPT_FIELD(CameraComponent, viewport_x),
                         PSYNDER_SCRIPT_FIELD(CameraComponent, viewport_y),
                         PSYNDER_SCRIPT_FIELD(CameraComponent, viewport_w),
                         PSYNDER_SCRIPT_FIELD(CameraComponent, viewport_h),
                         PSYNDER_SCRIPT_FIELD(CameraComponent, near_z),
                         PSYNDER_SCRIPT_FIELD(CameraComponent, far_z),
                         PSYNDER_SCRIPT_FIELD(CameraComponent, fov_y_rad),
                         PSYNDER_SCRIPT_FIELD(CameraComponent, aspect));

PSYNDER_SCRIPT_COMPONENT(VisibleBit, PSYNDER_SCRIPT_FIELD(VisibleBit, partition));

PSYNDER_SCRIPT_SYSTEM(advance_movers, &advance_movers, PSYNDER_SCRIPT_NAMES(),
                      PSYNDER_SCRIPT_NAMES("TransformWS"));
