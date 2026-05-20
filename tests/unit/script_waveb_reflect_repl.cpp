// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 20 (script) Wave B unit tests. Covers:
//   1. The PSYNDER_SCRIPT_COMPONENT macro: a POD declared once is reflected
//      into Lua via the `reflect` global with correct field kinds / offsets
//      and a real engine ECS id (scene::component_id<T>()).
//   2. The frozen GX render components (engine/scene/GxComponents.h) showing
//      up in `reflect` with the layout the static_asserts pin.
//   3. The PSYNDER_SCRIPT_SYSTEM macro: a system's schedule contract
//      (name + reads/writes) is reflected, including empty sets.
//   4. The REPL host: the `lua` console command evaluates a line against the
//      live Vm and Lua print() output is captured into the console.

#include "core/console/Console.h"
#include "script/Reflect.h"
#include "script/Script.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

// A POD gameplay component declared the Wave-B way. Field names avoid `max`
// so no platform min/max macro can interfere.
struct Health {
    psynder::f32 current;
    psynder::f32 peak;
    psynder::u32 team;
};

}  // namespace

PSYNDER_SCRIPT_COMPONENT(Health, PSYNDER_SCRIPT_FIELD(Health, current),
                         PSYNDER_SCRIPT_FIELD(Health, peak),
                         PSYNDER_SCRIPT_FIELD(Health, team));

namespace {

// A system declared the Wave-B way: reads Health, writes nothing.
void regen_system(psynder::scene::World& /*world*/, psynder::f64 /*dt*/) {}

}  // namespace

PSYNDER_SCRIPT_SYSTEM(regen_system, &regen_system, PSYNDER_SCRIPT_NAMES("Health"),
                      PSYNDER_SCRIPT_NAMES());

namespace {

class ReflectVmFixture {
public:
    ReflectVmFixture() { REQUIRE(psynder::script::Vm::Get().start()); }
    ~ReflectVmFixture() { psynder::script::Vm::Get().shutdown(); }

    psynder::script::Vm& vm() { return psynder::script::Vm::Get(); }
};

}  // namespace

TEST_CASE("script: reflect exposes a macro-declared component", "[script][reflect]") {
    ReflectVmFixture fix;
    std::string out;

    REQUIRE(fix.vm().execute_repl("type(reflect)", out));
    REQUIRE(out == "table");

    REQUIRE(fix.vm().execute_repl("reflect.component('Health') ~= nil", out));
    REQUIRE(out == "true");

    // Three fields, declared in order: current(f32), peak(f32), team(u32).
    REQUIRE(fix.vm().execute_repl("#reflect.component('Health').fields", out));
    REQUIRE(out == "3");

    REQUIRE(fix.vm().execute_repl("reflect.component('Health').fields[1].name", out));
    REQUIRE(out == "current");
    REQUIRE(fix.vm().execute_repl("reflect.component('Health').fields[1].kind", out));
    REQUIRE(out == "f32");
    REQUIRE(fix.vm().execute_repl("reflect.component('Health').fields[3].kind", out));
    REQUIRE(out == "u32");

    // Offsets/size reflect the real C++ layout, not Lua guesses.
    REQUIRE(fix.vm().execute_repl("reflect.component('Health').fields[2].offset", out));
    REQUIRE(out == "4");
    REQUIRE(fix.vm().execute_repl("reflect.component('Health').fields[3].offset", out));
    REQUIRE(out == "8");
    REQUIRE(fix.vm().execute_repl("reflect.component('Health').size", out));
    REQUIRE(out == "12");

    // The engine ECS id rode along from scene::component_id<Health>().
    REQUIRE(fix.vm().execute_repl("type(reflect.component('Health').id)", out));
    REQUIRE(out == "number");
    REQUIRE(fix.vm().execute_repl("reflect.component('Health').id ~= 0", out));
    REQUIRE(out == "true");
    // Distinct components carry distinct engine ids.
    REQUIRE(fix.vm().execute_repl(
        "reflect.component('Health').id ~= reflect.component('LightPoint').id", out));
    REQUIRE(out == "true");

    // An unknown component reflects as nil, not an error.
    REQUIRE(fix.vm().execute_repl("reflect.component('NotAComponent')", out));
    REQUIRE(out == "nil");

    // The reflect global is read-only: writes are rejected, not silently kept.
    REQUIRE_FALSE(fix.vm().execute_repl("reflect.components = nil", out));
    REQUIRE(out.find("read-only") != std::string::npos);
}

TEST_CASE("script: reflect exposes the frozen GX components", "[script][reflect]") {
    ReflectVmFixture fix;
    std::string out;

    // TransformWS: first field is the model-to-world Mat4; layout is frozen.
    REQUIRE(fix.vm().execute_repl("reflect.component('TransformWS').fields[1].kind", out));
    REQUIRE(out == "mat4");
    REQUIRE(fix.vm().execute_repl("reflect.component('TransformWS').size", out));
    REQUIRE(out == "128");

    // LightPoint: position is a Vec3 (12 bytes) at offset 0.
    REQUIRE(fix.vm().execute_repl("reflect.component('LightPoint').fields[1].kind", out));
    REQUIRE(out == "vec3");
    REQUIRE(fix.vm().execute_repl("reflect.component('LightPoint').fields[1].size", out));
    REQUIRE(out == "12");

    // MeshRef.mesh is a typed asset Handle (u32 newtype).
    REQUIRE(fix.vm().execute_repl("reflect.component('MeshRef').fields[1].kind", out));
    REQUIRE(out == "handle");

    // reflect.components() lists the registered set.
    REQUIRE(fix.vm().execute_repl(
        "(function()\n"
        "  for _, n in ipairs(reflect.components()) do\n"
        "    if n == 'CameraComponent' then return true end\n"
        "  end\n"
        "  return false\n"
        "end)()",
        out));
    REQUIRE(out == "true");
}

TEST_CASE("script: reflect exposes system schedule contracts", "[script][reflect]") {
    ReflectVmFixture fix;
    std::string out;

    // Macro-declared test system: reads {Health}, writes {}.
    REQUIRE(fix.vm().execute_repl("reflect.system('regen_system') ~= nil", out));
    REQUIRE(out == "true");
    REQUIRE(fix.vm().execute_repl("#reflect.system('regen_system').reads", out));
    REQUIRE(out == "1");
    REQUIRE(fix.vm().execute_repl("reflect.system('regen_system').reads[1]", out));
    REQUIRE(out == "Health");
    REQUIRE(fix.vm().execute_repl("#reflect.system('regen_system').writes", out));
    REQUIRE(out == "0");

    // The GX sample system: reads {}, writes {TransformWS}.
    REQUIRE(fix.vm().execute_repl("#reflect.system('advance_movers').reads", out));
    REQUIRE(out == "0");
    REQUIRE(fix.vm().execute_repl("reflect.system('advance_movers').writes[1]", out));
    REQUIRE(out == "TransformWS");

    REQUIRE(fix.vm().execute_repl("reflect.system('no_such_system')", out));
    REQUIRE(out == "nil");
}

TEST_CASE("script: lua console command evaluates against the live VM",
          "[script][repl][console]") {
    ReflectVmFixture fix;  // start() registers the `lua` console command
    auto& con = psynder::console::Console::Get();

    // Expression result is echoed to the console output.
    auto r = con.Execute("lua 1 + 2");
    REQUIRE(r.ok);
    REQUIRE(r.output.find("3") != std::string::npos);

    // Lua print() output is captured into the console output during the command.
    auto r2 = con.Execute("lua print('hello-repl')");
    REQUIRE(r2.ok);
    REQUIRE(r2.output.find("hello-repl") != std::string::npos);

    // The REPL sees engine reflection too — evaluated live against the world.
    auto r3 = con.Execute("lua reflect.component('Health').fields[3].kind");
    REQUIRE(r3.ok);
    REQUIRE(r3.output.find("u32") != std::string::npos);
}
