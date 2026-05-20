// SPDX-License-Identifier: MIT
// Psynder — diagnostics console surface. Wires the allocator heatmap and the
// flight recorder up to the existing console so they're dumpable on demand
// (the editor profiler panel and the network IPC layer both go through the
// console). The engine calls this once at startup, after the console exists.

#pragma once

namespace psynder::console {
class Console;
}

namespace psynder::diag {

// Register the diagnostics commands on `con`:
//   mem_heatmap [reset]                  print the heatmap / reset arena peaks
//   flightrecorder [clear | save <path>] dump / clear / save the event ring
// Idempotent: each command is skipped if it is already registered.
void register_console_commands(console::Console& con);

}  // namespace psynder::diag
