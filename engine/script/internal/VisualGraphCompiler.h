// SPDX-License-Identifier: MIT
// Psynder-GX script lane -- visual graph compiler.
//
// This is deliberately lane-internal. The public script VM contract remains
// Script.h; editor WebSocket clients can feed graph documents through the
// existing console REPL path using `:graph <json>`.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/Types.h"

#include "script/behavior/BehaviorIR.h"

namespace psynder::script::detail {

struct VisualCompileResult {
    bool ok = false;
    // Transitional live-preview output for the current Lua-backed editor VM.
    std::string lua_source;
    // Debuggable middle layer: PsyGraph lowers here before native/codegen paths.
    std::string psyscript_source;
    // Native output artifact for cook/build. This is C++ source text today;
    // later lanes can route eligible systems into CPU SIMD or compute kernels.
    std::string cpp_source;
    std::string diagnostic;
};

// Accepts:
//   :graph { "nodes": [...] }
//   :vscript { "nodes": [...] }
//   :visual { "nodes": [...] }
bool is_visual_graph_repl_command(std::string_view line) noexcept;

// Removes the leading REPL command and compiles the remaining graph payload.
VisualCompileResult compile_visual_graph_repl(std::string_view line);

// Compiles a graph document directly. Current wire shape:
// {
//   "nodes": [
//     {"id":"a", "op":"const", "value":1},
//     {"id":"b", "op":"const", "value":2},
//     {"id":"sum", "op":"add", "inputs":["a","b"]},
//     {"id":"out", "op":"log", "input":"sum"}
//   ],
//   "return": "sum"
// }
//
// Supported ops: const, add, sub, mul, div, neg, component, spawn, log, spin,
// return. The output is ordinary Lua source and runs in the existing safe
// Lua VM with the current `world` bindings. Node inputs are resolved in
// document order, so a node may only reference ids from earlier nodes; the
// top-level "return" field is evaluated after all nodes have been compiled.
VisualCompileResult compile_visual_graph(std::string_view graph_json);

// File/source routing helpers. `.vsg` and `.psygraph` are visual graph
// documents; `.psy` is reserved for the native Psynder VM.
bool is_visual_graph_name(std::string_view name) noexcept;
bool has_visual_graph_marker(std::string_view source) noexcept;
std::string_view strip_visual_graph_marker(std::string_view source) noexcept;

// ── PsyGraph → executable Behavior IR (ADR-018) ─────────────────────────────
// Lowers a graph document directly into a deterministic behavior::BehaviorProgram
// (engine/script/behavior/BehaviorIR) that runs over SoA component columns — the
// native-DOTS path, distinct from the Lua text `compile_visual_graph` emits.
//
// Node wire shape (each node -> one IR register; inputs reference EARLIER nodes):
//   {"id":"hp",   "op":"input",  "stream":"health"}
//   {"id":"thr",  "op":"const",  "value":25}
//   {"id":"low",  "op":"cmple",  "inputs":["hp","thr"]}
//   {"id":"heal", "op":"const",  "value":50}
//   {"id":"hp2",  "op":"add",    "inputs":["hp","heal"]}
//   {"id":"sel",  "op":"select", "inputs":["low","hp2","hp"]}
//   {"id":"w",    "op":"output", "stream":"health", "input":"sel"}
// Ops: input, const, output, add, sub, mul, div, neg, min, max,
//      cmple, cmplt, cmpge, cmpgt, select. `streams[i]` names stream id i so the
// caller can bind ECS component columns to the program's stream slots.
struct GraphIrResult {
    bool                       ok = false;
    behavior::BehaviorProgram  program;
    std::vector<std::string>   streams;  // index = stream id, value = its name
    std::string                diagnostic;
};
GraphIrResult lower_graph_to_ir(std::string_view graph_json);

}  // namespace psynder::script::detail
