// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — .psydem demo writer. Lane 18 (net).
//
// DemoWriter appends records to a .psydem file as the server runs. When the
// match ends (or the server shuts down), the caller invokes finalise() to
// flush the TOC and footer so the file is seekable.
//
// Thread-safety: NOT thread-safe. The caller must serialise writes onto the
// net thread (or take a lock). The server tick loop is single-threaded per
// the architecture in DESIGN-PSYNDER-GX.md §10.4.
//
// Wave B stub: all methods are no-ops that return success. M6 will fill in
// the binary serialisation using DemoFormat.h layout.
//
// Compile flag: -fno-fast-math -ffp-contract=off (DESIGN §14).

#pragma once

#include "DemoFormat.h"
#include "TickConfig.h"
#include "core/Types.h"

#include <span>
#include <string_view>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────────
// DemoWriterConfig — passed to DemoWriter::open().
// ──────────────────────────────────────────────────────────────────────────────
struct DemoWriterConfig {
    const char* path          = nullptr;  // file path; must be non-null on open()
    TickConfig  tick_cfg;                 // determines tick_hz in the header
    u32         map_id        = 0;
    u32         server_build  = 0;
    u64         match_start_ns = 0;
    // Roster: caller fills in up to 64 entries before calling open().
    DemoRosterEntry roster[64] = {};
    u8              player_count = 0;
    // Keyframe interval: how many ticks between forced full snapshots.
    // Smaller = finer seek granularity, larger file; 16 is a reasonable default.
    u32 keyframe_interval = 16;
};

// ──────────────────────────────────────────────────────────────────────────────
// DemoWriter
// ──────────────────────────────────────────────────────────────────────────────
class DemoWriter {
public:
    DemoWriter() noexcept = default;
    ~DemoWriter() noexcept;

    DemoWriter(const DemoWriter&)            = delete;
    DemoWriter& operator=(const DemoWriter&) = delete;
    DemoWriter(DemoWriter&&)                 noexcept;
    DemoWriter& operator=(DemoWriter&&)      noexcept;

    // Open a new .psydem file at `cfg.path`. Writes the file header +
    // roster block. Returns false if the file cannot be created.
    // STUB: returns true without creating a file.
    bool open(const DemoWriterConfig& cfg) noexcept;

    // True if the writer has an open file.
    bool is_open() const noexcept { return open_; }

    // Append a frame (snapshot delta) record for `tick`.
    // `snapshot_bytes` is the full snapshot for this tick.
    // `baseline_tick`  is the tick whose snapshot will be XOR'd as the delta
    //                  base; use `tick` to force a keyframe.
    // STUB: no-op, returns true.
    bool write_frame(u32               tick,
                     u32               baseline_tick,
                     std::span<const u8> snapshot_bytes) noexcept;

    // Append an input record for `tick`. `inputs` is a span of PlayerInputEntry.
    // STUB: no-op, returns true.
    bool write_inputs(u32                             tick,
                      std::span<const PlayerInputEntry> inputs) noexcept;

    // Finalise the demo: serialise the TOC + footer and close the file.
    // After this returns, is_open() == false.
    // STUB: no-op.
    void finalise() noexcept;

    // Emergency close without writing TOC (e.g. server crash path).
    // The file will NOT be seekable but is otherwise intact up to the last
    // flushed record.
    void close_unfinished() noexcept;

    // Number of ticks recorded so far.
    u32 ticks_written() const noexcept { return ticks_written_; }

private:
    bool open_        = false;
    u32  ticks_written_ = 0;
    // M6: add FILE* or std::FILE*, TOC scratch buffer, baseline snapshot buffer,
    // keyframe interval counter.
};

}  // namespace psynder::net
