// SPDX-License-Identifier: MIT
// Psynder-GX — PsyGraph → Behavior IR lowering (engine/script/internal/
// VisualGraphCompiler::lower_graph_to_ir). Proves a node graph compiles to the
// deterministic executable IR and runs over SoA component columns: arithmetic, a
// threshold+select gameplay rule, multi-stream I/O, error handling, and
// determinism. This is the front-end half of ADR-018 (graph -> DOTS, not Lua).

#include "script/internal/VisualGraphCompiler.h"
#include "script/behavior/BehaviorIR.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <string>
#include <vector>

using namespace psynder;
using psynder::script::detail::GraphIrResult;
using psynder::script::detail::lower_graph_to_ir;
namespace beh = psynder::script::behavior;

namespace {
int stream_index(const GraphIrResult& r, const std::string& name) {
    for (usize i = 0; i < r.streams.size(); ++i)
        if (r.streams[i] == name) return static_cast<int>(i);
    return -1;
}
}  // namespace

TEST_CASE("graph-ir: an arithmetic graph lowers and computes v*2+1",
          "[graph][script]") {
    const char* g = R"({"nodes":[
        {"id":"v","op":"input","stream":"val"},
        {"id":"two","op":"const","value":2},
        {"id":"d","op":"mul","inputs":["v","two"]},
        {"id":"one","op":"const","value":1},
        {"id":"r","op":"add","inputs":["d","one"]},
        {"id":"o","op":"output","stream":"val","input":"r"}
    ]})";
    const GraphIrResult res = lower_graph_to_ir(g);
    REQUIRE(res.ok);
    REQUIRE(res.streams.size() == 1);
    const int val = stream_index(res, "val");
    REQUIRE(val == 0);

    beh::BehaviorChunk chunk;
    chunk.configure(res.program.num_streams, 3);
    chunk.stream(static_cast<u16>(val))[0] = 3.0f;
    chunk.stream(static_cast<u16>(val))[1] = 0.0f;
    chunk.stream(static_cast<u16>(val))[2] = -2.0f;
    beh::execute(res.program, chunk);
    REQUIRE(chunk.stream(static_cast<u16>(val))[0] == Catch::Approx(7.0f));   // 3*2+1
    REQUIRE(chunk.stream(static_cast<u16>(val))[1] == Catch::Approx(1.0f));   // 0*2+1
    REQUIRE(chunk.stream(static_cast<u16>(val))[2] == Catch::Approx(-3.0f));  // -2*2+1
}

TEST_CASE("graph-ir: a threshold+select gameplay rule heals low health",
          "[graph][script][gameplay]") {
    const char* g = R"({"nodes":[
        {"id":"hp","op":"input","stream":"health"},
        {"id":"thr","op":"const","value":25},
        {"id":"low","op":"cmple","inputs":["hp","thr"]},
        {"id":"heal","op":"const","value":50},
        {"id":"hp2","op":"add","inputs":["hp","heal"]},
        {"id":"sel","op":"select","inputs":["low","hp2","hp"]},
        {"id":"w","op":"output","stream":"health","input":"sel"}
    ]})";
    const GraphIrResult res = lower_graph_to_ir(g);
    REQUIRE(res.ok);
    const int hp = stream_index(res, "health");
    REQUIRE(hp >= 0);

    beh::BehaviorChunk chunk;
    chunk.configure(res.program.num_streams, 4);
    const f32 in[4] = {10.0f, 25.0f, 26.0f, 80.0f};
    for (usize i = 0; i < 4; ++i) chunk.stream(static_cast<u16>(hp))[i] = in[i];
    beh::execute(res.program, chunk);
    REQUIRE(chunk.stream(static_cast<u16>(hp))[0] == Catch::Approx(60.0f));  // healed
    REQUIRE(chunk.stream(static_cast<u16>(hp))[1] == Catch::Approx(75.0f));  // healed
    REQUIRE(chunk.stream(static_cast<u16>(hp))[2] == Catch::Approx(26.0f));  // untouched
    REQUIRE(chunk.stream(static_cast<u16>(hp))[3] == Catch::Approx(80.0f));  // untouched
}

TEST_CASE("graph-ir: a multi-stream graph reads two columns and writes a third",
          "[graph][script]") {
    const char* g = R"({"nodes":[
        {"id":"a","op":"input","stream":"a"},
        {"id":"b","op":"input","stream":"b"},
        {"id":"s","op":"add","inputs":["a","b"]},
        {"id":"o","op":"output","stream":"sum","input":"s"}
    ]})";
    const GraphIrResult res = lower_graph_to_ir(g);
    REQUIRE(res.ok);
    REQUIRE(res.streams.size() == 3);
    const int ia = stream_index(res, "a"), ib = stream_index(res, "b"),
              isum = stream_index(res, "sum");
    REQUIRE((ia >= 0 && ib >= 0 && isum >= 0));

    beh::BehaviorChunk chunk;
    chunk.configure(res.program.num_streams, 2);
    chunk.stream(static_cast<u16>(ia))[0] = 4.0f;
    chunk.stream(static_cast<u16>(ib))[0] = 5.0f;
    chunk.stream(static_cast<u16>(ia))[1] = -1.0f;
    chunk.stream(static_cast<u16>(ib))[1] = 10.0f;
    beh::execute(res.program, chunk);
    REQUIRE(chunk.stream(static_cast<u16>(isum))[0] == Catch::Approx(9.0f));
    REQUIRE(chunk.stream(static_cast<u16>(isum))[1] == Catch::Approx(9.0f));
}

TEST_CASE("graph-ir: malformed graphs fail with a diagnostic", "[graph][script]") {
    REQUIRE_FALSE(lower_graph_to_ir("{ not json").ok);
    REQUIRE_FALSE(lower_graph_to_ir(R"({"foo":1})").ok);  // no nodes array
    const GraphIrResult bad_op = lower_graph_to_ir(
        R"({"nodes":[{"id":"x","op":"frobnicate"}]})");
    REQUIRE_FALSE(bad_op.ok);
    REQUIRE_FALSE(bad_op.diagnostic.empty());
    // A binary op missing inputs is rejected.
    REQUIRE_FALSE(
        lower_graph_to_ir(R"({"nodes":[{"id":"x","op":"add"}]})").ok);
}

TEST_CASE("graph-ir: lowering is deterministic", "[graph][script][determinism]") {
    const char* g = R"({"nodes":[
        {"id":"v","op":"input","stream":"val"},
        {"id":"k","op":"const","value":3},
        {"id":"r","op":"mul","inputs":["v","k"]},
        {"id":"o","op":"output","stream":"val","input":"r"}
    ]})";
    const GraphIrResult a = lower_graph_to_ir(g);
    const GraphIrResult b = lower_graph_to_ir(g);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    REQUIRE(a.program.code.size() == b.program.code.size());
    REQUIRE(a.program.num_registers == b.program.num_registers);
    REQUIRE(a.program.num_streams == b.program.num_streams);
    REQUIRE(a.streams == b.streams);
}
