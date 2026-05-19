// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — .psydem demo reader. Lane 18 (net).
//
// DemoReader provides:
//   - O(log n) seek to any tick via the TOC at file end (DESIGN §10.4).
//   - Sequential iteration for replay playback.
//   - Full deterministic re-simulation feed: each tick delivers snapshot +
//     input records so the replay system can reconstruct the match state.
//
// Wave B stub: all methods return failure or empty. M6 fills in the binary
// parsing using DemoFormat.h.
//
// Compile flag: -fno-fast-math -ffp-contract=off (DESIGN §14).

#pragma once

#include "DemoFormat.h"
#include "TickConfig.h"
#include "core/Types.h"

#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────────
// DemoTickData — output from advance_tick() / seek_to_tick().
// The caller receives all records for one tick in one call.
// ──────────────────────────────────────────────────────────────────────────────
struct DemoTickData {
    u32 tick          = 0;
    bool has_frame    = false;   // true if snapshot data below is valid
    bool is_keyframe  = false;   // frame record was a keyframe (no XOR delta)

    // Decoded snapshot for this tick (after applying delta). The vector is
    // pre-allocated by the reader and reused across ticks.
    std::vector<u8> snapshot_bytes;

    // Player inputs for this tick.
    std::vector<PlayerInputEntry> inputs;
};

// ──────────────────────────────────────────────────────────────────────────────
// DemoReaderResult
// ──────────────────────────────────────────────────────────────────────────────
enum class DemoReaderResult : u8 {
    Ok             = 0,
    EndOfFile      = 1,  // No more ticks.
    VersionMismatch = 2,  // Demo file kDemoVersion != kDemoVersion constant.
    CorruptFile    = 3,  // Magic / structural integrity check failed.
    IoError        = 4,
};

// ──────────────────────────────────────────────────────────────────────────────
// DemoReader
// ──────────────────────────────────────────────────────────────────────────────
class DemoReader {
public:
    DemoReader() noexcept = default;
    ~DemoReader() noexcept;

    DemoReader(const DemoReader&)            = delete;
    DemoReader& operator=(const DemoReader&) = delete;
    DemoReader(DemoReader&&)                 noexcept;
    DemoReader& operator=(DemoReader&&)      noexcept;

    // Open and validate a .psydem file. Reads the header + roster + TOC footer.
    // Returns Corrupt or VersionMismatch early if the file is invalid.
    // STUB: returns IoError (file not yet implemented).
    DemoReaderResult open(const char* path) noexcept;

    // True if a file is open and ready for iteration.
    bool is_open() const noexcept { return open_; }

    // Parsed header (valid after open() returns Ok).
    const DemoFileHeader& header() const noexcept { return header_; }

    // Roster entries (valid after open() returns Ok).
    std::span<const DemoRosterEntry> roster() const noexcept {
        return {roster_.data(), roster_.size()};
    }

    // Tick config derived from header.tick_hz (valid after open() returns Ok).
    TickConfig tick_config() const noexcept { return tick_cfg_; }

    // Advance to the next tick in file order. Fills `out`.
    // Returns EndOfFile when all ticks have been delivered.
    // STUB: returns EndOfFile immediately.
    DemoReaderResult advance_tick(DemoTickData& out) noexcept;

    // O(log n) seek to `tick`. Uses the TOC binary search.
    // After this call, the next advance_tick() delivers `tick`.
    // Returns CorruptFile if `tick` is outside [start_tick, end_tick].
    // STUB: returns CorruptFile.
    DemoReaderResult seek_to_tick(u32 tick) noexcept;

    // Current playback position (tick number of the LAST delivered tick).
    // Returns 0 if no tick has been delivered yet.
    u32 current_tick() const noexcept { return current_tick_; }

    void close() noexcept;

private:
    bool            open_         = false;
    u32             current_tick_ = 0;
    DemoFileHeader  header_{};
    std::vector<DemoRosterEntry> roster_;
    std::vector<DemoTocEntry>    toc_;
    TickConfig      tick_cfg_{};
    // M6: add FILE*, baseline snapshot buffer for XOR reconstruction.
};

}  // namespace psynder::net
