// SPDX-License-Identifier: MIT
// Psynder — diagnostics console surface impl. See DiagCommands.h.

#include "DiagCommands.h"

#include "FlightRecorder.h"
#include "alloc/Heatmap.h"
#include "console/Console.h"

#include <span>
#include <string>
#include <string_view>

namespace psynder::diag {

void register_console_commands(console::Console& con) {
    if (con.FindCommand("mem_heatmap") == nullptr) {
        con.RegisterCommand(
            "mem_heatmap",
            "Print the allocator heatmap (per-tag + per-arena live/peak). "
            "'mem_heatmap reset' clears the per-arena peak watermarks.",
            [](std::span<const std::string_view> args, console::Output& out) {
                auto& hm = mem::Heatmap::get();
                if (!args.empty() && args[0] == "reset") {
                    hm.reset_peaks();
                    out.PrintLine("mem_heatmap: per-arena peaks reset");
                    return;
                }
                out.Print(hm.format());
            });
    }

    if (con.FindCommand("flightrecorder") == nullptr) {
        con.RegisterCommand(
            "flightrecorder",
            "Dump the flight recorder ring oldest-first. 'flightrecorder clear' "
            "empties it; 'flightrecorder save <path>' writes it to a file.",
            [](std::span<const std::string_view> args, console::Output& out) {
                auto& fr = FlightRecorder::get();
                if (!args.empty() && args[0] == "clear") {
                    fr.clear();
                    out.PrintLine("flightrecorder: cleared");
                    return;
                }
                if (!args.empty() && args[0] == "save") {
                    if (args.size() < 2) {
                        out.PrintLine("usage: flightrecorder save <path>");
                        return;
                    }
                    const std::string path(args[1]);
                    const bool ok = fr.dump_to_file(path.c_str());
                    out.FormatLine("flightrecorder: {} {}",
                                   ok ? "saved to" : "FAILED to save to", path);
                    return;
                }
                out.Print(fr.dump());
            });
    }
}

}  // namespace psynder::diag
