// SPDX-License-Identifier: MIT
// Psynder-GX visual-graph (PsyGraph) routing and codegen tests.

#include "script/Script.h"
#include "script/internal/VisualGraphCompiler.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

class VmFixture {
   public:
    VmFixture() { REQUIRE(psynder::script::Vm::Get().start()); }
    ~VmFixture() { psynder::script::Vm::Get().shutdown(); }

    psynder::script::Vm& vm() { return psynder::script::Vm::Get(); }
};

}  // namespace

TEST_CASE("script: visual graph REPL compiles through the transitional path",
          "[script][psygraph][repl]") {
    VmFixture fix;

    std::string out;
    REQUIRE(fix.vm().execute_repl(
        R"(:graph {"nodes":[{"id":"a","op":"const","value":2},{"id":"b","op":"const","value":3},{"id":"sum","op":"add","inputs":["a","b"]}],"return":"sum"})",
        out));
    REQUIRE(out == "5");
}

TEST_CASE("script: visual graph spin node registers a DOTS system",
          "[script][psygraph][dots]") {
    VmFixture fix;

    std::string out;
    REQUIRE(fix.vm().execute_repl(
        R"(:graph {"nodes":[{"id":"spin_crates","op":"spin","axis":[0,1,0],"speed":{"type":"linearIndex","base":0.35,"step":0.12},"phase":{"type":"constant","value":0},"targetGroup":"crates"}]})",
        out));

    REQUIRE(fix.vm().execute_repl("world:system_count()", out));
    REQUIRE(out == "1");
}

TEST_CASE("script: visual graph emits PsyScript IR and native C++",
          "[script][psygraph][codegen]") {
    const auto compiled = psynder::script::detail::compile_visual_graph(
        R"({"nodes":[{"id":"spin_crates","op":"spin","axis":[0,1,0],"speed":{"type":"linearIndex","base":0.35,"step":0.12},"phase":{"type":"constant","value":0},"targetGroup":"crates"}]})");

    REQUIRE(compiled.ok);
    REQUIRE(compiled.lua_source.find("world:register_system") != std::string::npos);
    REQUIRE(compiled.psyscript_source.find("# psynder-script-ir v0") != std::string::npos);
    REQUIRE(compiled.psyscript_source.find("execution = cpu_simd_candidate") !=
            std::string::npos);
    REQUIRE(compiled.cpp_source.find("struct PsyGraphSpin_spin_crates") !=
            std::string::npos);
    REQUIRE(compiled.cpp_source.find("static void run") != std::string::npos);
    REQUIRE(compiled.cpp_source.find("0.35f + 0.12f * static_cast<float>(i)") !=
            std::string::npos);
}
