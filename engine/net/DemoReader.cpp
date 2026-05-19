// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — .psydem demo reader implementation (Wave B stub). Lane 18 (net).
//
// All methods are stubs. M6 will implement:
//   - Binary parsing of DemoFileHeader + DemoRosterEntry[].
//   - TOC footer parse + binary search for seek_to_tick().
//   - Sequential record decode (XOR delta application) for advance_tick().
//
// Compile flags required on this TU:
//   -fno-fast-math -ffp-contract=off
// (DESIGN-PSYNDER-GX.md §14 — lockstep determinism.)

#include "DemoReader.h"

namespace psynder::net {

DemoReader::~DemoReader() noexcept {
    if (open_) close();
}

DemoReader::DemoReader(DemoReader&& o) noexcept
    : open_(o.open_)
    , current_tick_(o.current_tick_)
    , header_(o.header_)
    , roster_(std::move(o.roster_))
    , toc_(std::move(o.toc_))
    , tick_cfg_(o.tick_cfg_)
{
    o.open_         = false;
    o.current_tick_ = 0;
}

DemoReader& DemoReader::operator=(DemoReader&& o) noexcept {
    if (this != &o) {
        if (open_) close();
        open_         = o.open_;
        current_tick_ = o.current_tick_;
        header_       = o.header_;
        roster_       = std::move(o.roster_);
        toc_          = std::move(o.toc_);
        tick_cfg_     = o.tick_cfg_;
        o.open_         = false;
        o.current_tick_ = 0;
    }
    return *this;
}

DemoReaderResult DemoReader::open(const char* path) noexcept {
    (void)path;
    // M6:
    //   1. fopen(path, "rb"); check for error -> IoError.
    //   2. fseek to EOF - sizeof(DemoTocFooter); read footer.
    //   3. Validate footer.footer_magic == kDemoMagic -> CorruptFile.
    //   4. fseek to footer.toc_offset; read footer.toc_count DemoTocEntry records.
    //   5. fseek to 0; read DemoFileHeader.
    //   6. Validate header.magic == kDemoMagic -> CorruptFile.
    //   7. Validate header.demo_version == kDemoVersion -> VersionMismatch.
    //   8. Read header.player_count DemoRosterEntry records.
    //   9. Derive tick_cfg_ from header.tick_hz.
    //  10. Set current_tick_ = header.start_tick - 1 (before first advance).
    return DemoReaderResult::IoError;
}

DemoReaderResult DemoReader::advance_tick(DemoTickData& out) noexcept {
    if (!open_) return DemoReaderResult::IoError;
    (void)out;
    // M6:
    //   1. Read DemoRecordHeader at current file position.
    //   2. Dispatch on type:
    //      - kRecordTypeFrame  -> decode DemoFrameRecordPreamble + delta;
    //        if baseline_tick == tick: copy to baseline buffer (keyframe);
    //        else XOR delta_data against baseline buffer;
    //        store result in out.snapshot_bytes.
    //      - kRecordTypeInput  -> decode DemoInputPreamble + PlayerInputEntry[];
    //        push into out.inputs.
    //      - kRecordTypeEvent  -> skip (reserved for M7).
    //   3. When all records for a tick have been consumed, set current_tick_
    //      and return Ok.
    return DemoReaderResult::EndOfFile;
}

DemoReaderResult DemoReader::seek_to_tick(u32 tick) noexcept {
    if (!open_) return DemoReaderResult::IoError;
    // M6:
    //   1. Binary-search toc_ for the largest entry whose tick <= requested tick.
    //   2. fseek to that entry's file_offset.
    //   3. Reset baseline buffer to zeroes (the TOC entry is always a keyframe).
    //   4. Set current_tick_ = toc_entry.tick - 1 (advance_tick() delivers it next).
    //   5. If tick < header_.start_tick or tick > header_.end_tick: CorruptFile.
    (void)tick;
    return DemoReaderResult::CorruptFile;
}

void DemoReader::close() noexcept {
    if (!open_) return;
    // M6: fclose; clear roster_, toc_.
    open_         = false;
    current_tick_ = 0;
    roster_.clear();
    toc_.clear();
}

}  // namespace psynder::net
