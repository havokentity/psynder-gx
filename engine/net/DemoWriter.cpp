// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — .psydem demo writer implementation (Wave B stub). Lane 18 (net).
//
// All methods are no-ops that return success. M6 will implement binary
// serialisation using the layout defined in DemoFormat.h.
//
// Compile flags required on this TU:
//   -fno-fast-math -ffp-contract=off
// (DESIGN-PSYNDER-GX.md §14 — lockstep determinism.)

#include "DemoWriter.h"

namespace psynder::net {

DemoWriter::~DemoWriter() noexcept {
    if (open_) {
        close_unfinished();
    }
}

DemoWriter::DemoWriter(DemoWriter&& o) noexcept
    : open_(o.open_)
    , ticks_written_(o.ticks_written_)
{
    o.open_          = false;
    o.ticks_written_ = 0;
}

DemoWriter& DemoWriter::operator=(DemoWriter&& o) noexcept {
    if (this != &o) {
        if (open_) close_unfinished();
        open_          = o.open_;
        ticks_written_ = o.ticks_written_;
        o.open_          = false;
        o.ticks_written_ = 0;
    }
    return *this;
}

bool DemoWriter::open(const DemoWriterConfig& cfg) noexcept {
    (void)cfg;
    // M6: create file at cfg.path, encode DemoFileHeader + roster block,
    // initialise the baseline snapshot buffer to zeroes, reset the TOC
    // scratch list.
    open_          = true;
    ticks_written_ = 0;
    return true;
}

bool DemoWriter::write_frame(u32                 tick,
                              u32                 baseline_tick,
                              std::span<const u8> snapshot_bytes) noexcept {
    (void)tick;
    (void)baseline_tick;
    (void)snapshot_bytes;
    // M6:
    //   1. If (tick - last_keyframe_tick >= keyframe_interval): force keyframe
    //      (baseline_tick = tick; store full snapshot; append TOC entry).
    //   2. Otherwise: XOR snapshot_bytes against stored baseline buffer.
    //   3. Write DemoRecordHeader (kRecordTypeFrame) + DemoFrameRecordPreamble
    //      + delta payload to file.
    //   4. Update stored baseline buffer.
    if (!open_) return false;
    ++ticks_written_;
    return true;
}

bool DemoWriter::write_inputs(
    u32                               tick,
    std::span<const PlayerInputEntry> inputs) noexcept
{
    (void)tick;
    (void)inputs;
    // M6: write DemoRecordHeader (kRecordTypeInput) + DemoInputPreamble +
    // PlayerInputEntry[] to file.
    if (!open_) return false;
    return true;
}

void DemoWriter::finalise() noexcept {
    if (!open_) return;
    // M6:
    //   1. Serialise TOC array (DemoTocEntry per keyframe).
    //   2. Record the file offset of the TOC start.
    //   3. Write DemoTocFooter at file end.
    //   4. Update DemoFileHeader.end_tick in-place (pwrite/fseek+fwrite).
    //   5. fclose.
    open_ = false;
}

void DemoWriter::close_unfinished() noexcept {
    if (!open_) return;
    // M6: fclose without TOC; file is partially usable via sequential scan.
    open_ = false;
}

}  // namespace psynder::net
