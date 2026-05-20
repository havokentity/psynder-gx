// SPDX-License-Identifier: MIT
// Psynder-GX — script-lane reflection + DOTS registration macros.
//
// NOT part of the frozen Psynder public surface (Script.h). This is a GX
// extension header (sibling to ScriptGx.h) that any lane MAY include to expose
// its POD components / free-function systems to Lua with one declaration.
//
// ─── "Declare once, get Lua bindings + minimal reflection" ─────────────────
//
// Components — declare in a .cpp at namespace scope. The component type must
// be a complete, in-scope, *unqualified* identifier so the macro can both
// stringify it (Lua-visible name) and token-paste it (storage symbol):
//
//   using namespace psynder::scene;        // so `LightPoint` is unqualified
//   PSYNDER_SCRIPT_COMPONENT(LightPoint,
//       PSYNDER_SCRIPT_FIELD(LightPoint, position),
//       PSYNDER_SCRIPT_FIELD(LightPoint, radius),
//       PSYNDER_SCRIPT_FIELD(LightPoint, color),
//       PSYNDER_SCRIPT_FIELD(LightPoint, intensity));
//
// The macro also calls scene::component_id<T>() so the engine ECS id travels
// with the script-side schema — one declaration feeds both sides.
//
// Systems — declare the schedule contract (name + reads/writes + fn):
//
//   void advance_movers(scene::World& w, f64 dt);
//   PSYNDER_SCRIPT_SYSTEM(advance_movers, &advance_movers,
//       PSYNDER_SCRIPT_NAMES(),                  // reads:  none
//       PSYNDER_SCRIPT_NAMES("TransformWS"));    // writes: TransformWS
//
// After Vm::start(), the running interpreter exposes a `reflect` global:
//
//   reflect.components()              -> { 'LightPoint', 'TransformWS', ... }
//   reflect.component('LightPoint')   -> { name=, id=, size=, align=,
//                                          fields = { { name='position',
//                                              kind='vec3', offset=0,
//                                              size=12 }, ... } }
//   reflect.systems()                 -> { 'advance_movers', ... }
//   reflect.system('advance_movers')  -> { name=, reads={}, writes={'TransformWS'} }
//
// Registration happens at static-init time; the registries are process-wide
// and queried by the binding layer. A component must declare >= 1 field.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "scene/World.h"

#include <cstddef>
#include <initializer_list>
#include <span>
#include <string_view>
#include <type_traits>

namespace psynder::script {

// Field type tag. Stable, ASCII, lowercase string forms (see kind_name) double
// as Lua keys. Aggregates that the engine actually stores in components get a
// dedicated kind; anything else (enums, nested PODs) reflects as `Unknown` but
// still carries a correct offset/size.
enum class FieldKind : u8 {
    Unknown = 0,
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    F32,
    F64,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Mat3,
    Mat4,
    IVec2,
    IVec3,
    Handle,
};

// Lowercase ASCII name for a kind (e.g. FieldKind::Vec3 -> "vec3"). Never null.
const char* kind_name(FieldKind k) noexcept;

struct FieldDesc {
    const char* name;
    FieldKind   kind;
    u32         offset;  // byte offset of the field within the component
    u32         size;    // byte size of the field
};

struct ComponentReflection {
    const char*                name;
    scene::ComponentId         id;     // engine ECS id (scene::component_id<T>())
    u32                        size;   // sizeof(T)
    u32                        align;  // alignof(T)
    std::span<const FieldDesc> fields;  // points at static storage; never owns
};

// A C++ system is a free function over the live world. The reflection captures
// the schedule contract (name + declared reads/writes) so the scheduler and
// the editor REPL can introspect it; `fn` may be null for metadata-only entries.
using SystemFn = void (*)(scene::World& world, f64 dt);

struct SystemReflection {
    const char*                  name;
    std::span<const char* const> reads;   // component type names (read set)
    std::span<const char* const> writes;  // component type names (write set)
    SystemFn                     fn;
};

// ─── Registry (process-wide, populated at static init) ─────────────────────
// Registration is idempotent by name: re-registering a name returns the first
// entry unchanged. `fields` / `reads` / `writes` must point at storage with
// static lifetime (the macros emit static arrays / string literals).

const ComponentReflection* register_component_reflection(const char* name,
                                                         scene::ComponentId id,
                                                         u32              size,
                                                         u32              align,
                                                         const FieldDesc* fields,
                                                         u32 field_count);

const ComponentReflection* find_component_reflection(std::string_view name) noexcept;
std::span<const ComponentReflection* const> all_component_reflections() noexcept;

const SystemReflection* register_system_reflection(
    const char* name, SystemFn fn, std::initializer_list<const char*> reads,
    std::initializer_list<const char*> writes);

const SystemReflection* find_system_reflection(std::string_view name) noexcept;
std::span<const SystemReflection* const> all_system_reflections() noexcept;

// ─── Compile-time field-kind deduction ─────────────────────────────────────
template <class T>
inline constexpr FieldKind field_kind_v = FieldKind::Unknown;

// clang-format off
template <> inline constexpr FieldKind field_kind_v<i8>   = FieldKind::I8;
template <> inline constexpr FieldKind field_kind_v<i16>  = FieldKind::I16;
template <> inline constexpr FieldKind field_kind_v<i32>  = FieldKind::I32;
template <> inline constexpr FieldKind field_kind_v<i64>  = FieldKind::I64;
template <> inline constexpr FieldKind field_kind_v<u8>   = FieldKind::U8;
template <> inline constexpr FieldKind field_kind_v<u16>  = FieldKind::U16;
template <> inline constexpr FieldKind field_kind_v<u32>  = FieldKind::U32;
template <> inline constexpr FieldKind field_kind_v<u64>  = FieldKind::U64;
template <> inline constexpr FieldKind field_kind_v<f32>  = FieldKind::F32;
template <> inline constexpr FieldKind field_kind_v<f64>  = FieldKind::F64;
template <> inline constexpr FieldKind field_kind_v<bool> = FieldKind::Bool;
template <> inline constexpr FieldKind field_kind_v<math::Vec2>  = FieldKind::Vec2;
template <> inline constexpr FieldKind field_kind_v<math::Vec3>  = FieldKind::Vec3;
template <> inline constexpr FieldKind field_kind_v<math::Vec4>  = FieldKind::Vec4;
template <> inline constexpr FieldKind field_kind_v<math::Quat>  = FieldKind::Quat;
template <> inline constexpr FieldKind field_kind_v<math::Mat3>  = FieldKind::Mat3;
template <> inline constexpr FieldKind field_kind_v<math::Mat4>  = FieldKind::Mat4;
template <> inline constexpr FieldKind field_kind_v<math::IVec2> = FieldKind::IVec2;
template <> inline constexpr FieldKind field_kind_v<math::IVec3> = FieldKind::IVec3;
// clang-format on

// psynder::Handle<Tag> is a u32 newtype used for asset / resource ids.
template <class Tag>
inline constexpr FieldKind field_kind_v<Handle<Tag>> = FieldKind::Handle;

}  // namespace psynder::script

// ─── Macros ────────────────────────────────────────────────────────────────

// Build a FieldDesc for `Type::Member`. `Type` is used only inside
// decltype / offsetof / sizeof, so it MAY be namespace-qualified here.
// decltype(Type::Member) naming a non-static data member is well-formed in an
// unevaluated operand (the canonical reflection idiom); cv/ref are stripped
// before the field-kind lookup so a `const`/reference member still maps.
#define PSYNDER_SCRIPT_FIELD(Type, Member)                                  \
    ::psynder::script::FieldDesc {                                          \
        #Member,                                                            \
            ::psynder::script::field_kind_v<::std::remove_cv_t<             \
                ::std::remove_reference_t<decltype(Type::Member)>>>,        \
            static_cast<::psynder::u32>(offsetof(Type, Member)),            \
            static_cast<::psynder::u32>(sizeof(decltype(Type::Member)))     \
    }

// Register reflection for an in-scope, unqualified POD component `Type`.
// `#Type` is the Lua-visible name; component_id<Type>() ties the engine id.
// Use at namespace scope only. The first field is a REQUIRED argument, so a
// component with no fields is rejected by the preprocessor rather than
// silently registered as an empty schema (this also keeps the field array
// non-empty, avoiding the ill-formed zero-size-array case).
#define PSYNDER_SCRIPT_COMPONENT(Type, Field1, ...)                         \
    namespace {                                                             \
    static_assert(::std::is_standard_layout_v<Type>,                        \
                  "PSYNDER_SCRIPT_COMPONENT requires a standard-layout "    \
                  "type (offsetof is UB otherwise)");                       \
    [[maybe_unused]] const ::psynder::script::FieldDesc                     \
        psy_refl_fields_##Type[] = {Field1 __VA_OPT__(, ) __VA_ARGS__};     \
    [[maybe_unused]] const ::psynder::script::ComponentReflection* const    \
        psy_refl_##Type = ::psynder::script::register_component_reflection( \
            #Type, ::psynder::scene::component_id<Type>(),                  \
            static_cast<::psynder::u32>(sizeof(Type)),                      \
            static_cast<::psynder::u32>(alignof(Type)),                     \
            psy_refl_fields_##Type,                                         \
            static_cast<::psynder::u32>(sizeof(psy_refl_fields_##Type) /    \
                                        sizeof(psy_refl_fields_##Type[0]))); \
    }  // namespace

// Wrap a comma-separated list of component-name string literals for the
// reads / writes arguments of PSYNDER_SCRIPT_SYSTEM. May be empty.
#define PSYNDER_SCRIPT_NAMES(...) \
    ::std::initializer_list<const char*> { __VA_ARGS__ }

// Register a C++ system's schedule contract. `Tag` is a paste-safe identifier
// (the Lua-visible name); `Fn` is a SystemFn (or nullptr). Use at namespace
// scope only.
#define PSYNDER_SCRIPT_SYSTEM(Tag, Fn, Reads, Writes)                       \
    namespace {                                                             \
    [[maybe_unused]] const ::psynder::script::SystemReflection* const       \
        psy_sysrefl_##Tag = ::psynder::script::register_system_reflection(  \
            #Tag, Fn, Reads, Writes);                                       \
    }  // namespace
