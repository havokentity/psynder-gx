// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — .psydem demo writer impl. Lane 18 (net).
//
// File layout per DemoFormat.h:
//   - DemoFileHeader  (64 bytes)
//   - DemoRosterEntry[player_count]
//   - DemoRecord*    (frame / input / event records, sequential)
//   - DemoTocEntry[toc_count]
//   - DemoTocFooter  (16 bytes, at file end)
//
// All fixed-size structs are POD-ish and written directly as bytes; the
// engine is little-endian-only (DESIGN §11.5: x86-64 + Apple Silicon).
//
// Delta compression: frames carry a XOR-delta against the most recent
// keyframe baseline (SnapshotDelta.h). The writer forces a keyframe every
// `cfg.keyframe_interval` ticks regardless of caller hint, so the TOC has
// reasonable seek granularity.

#include "DemoWriter.h"

#include "SnapshotDelta.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace psynder::net {

namespace {

PSY_FORCEINLINE std::FILE* as_file(void* p) noexcept {
    return static_cast<std::FILE*>(p);
}

bool fwrite_all(std::FILE* f, const void* data, usize n) noexcept {
    return std::fwrite(data, 1, n, f) == n;
}

}  // namespace

DemoWriter::~DemoWriter() noexcept {
    if (file_) {
        close_unfinished();
    }
}

DemoWriter::DemoWriter(DemoWriter&& o) noexcept
    : file_(o.file_)
    , ticks_written_(o.ticks_written_)
    , keyframe_interval_(o.keyframe_interval_)
    , start_tick_(o.start_tick_)
    , last_tick_(o.last_tick_)
    , last_keyframe_tick_(o.last_keyframe_tick_)
    , baseline_(std::move(o.baseline_))
    , toc_(std::move(o.toc_))
    , scratch_(std::move(o.scratch_))
{
    o.file_              = nullptr;
    o.ticks_written_     = 0;
    o.last_keyframe_tick_ = 0;
}

DemoWriter& DemoWriter::operator=(DemoWriter&& o) noexcept {
    if (this != &o) {
        if (file_) close_unfinished();
        file_              = o.file_;
        ticks_written_     = o.ticks_written_;
        keyframe_interval_ = o.keyframe_interval_;
        start_tick_        = o.start_tick_;
        last_tick_         = o.last_tick_;
        last_keyframe_tick_ = o.last_keyframe_tick_;
        baseline_          = std::move(o.baseline_);
        toc_               = std::move(o.toc_);
        scratch_           = std::move(o.scratch_);
        o.file_              = nullptr;
        o.ticks_written_     = 0;
        o.last_keyframe_tick_ = 0;
    }
    return *this;
}

bool DemoWriter::open(const DemoWriterConfig& cfg) noexcept {
    if (file_) return false;
    if (!cfg.path) return false;

    // "wb+" so finalise() can fread() the header back to patch end_tick.
    // "wb" is write-only and the patch path would silently fail.
    std::FILE* f = std::fopen(cfg.path, "wb+");
    if (!f) return false;

    DemoFileHeader hdr{};
    hdr.magic           = kDemoMagic;
    hdr.demo_version    = kDemoVersion;
    hdr.tick_hz         = static_cast<u8>(cfg.tick_cfg.tick_hz);
    hdr.map_id          = cfg.map_id;
    hdr.server_build    = cfg.server_build;
    hdr.match_start_ns  = cfg.match_start_ns;
    hdr.start_tick      = 0;
    hdr.end_tick        = 0;  // patched in finalise()
    hdr.player_count    = cfg.player_count;
    hdr.roster_offset   = sizeof(DemoFileHeader);
    hdr.first_record_offset =
        static_cast<u32>(sizeof(DemoFileHeader)
                         + static_cast<usize>(cfg.player_count) * sizeof(DemoRosterEntry));

    if (!fwrite_all(f, &hdr, sizeof(hdr))) {
        std::fclose(f);
        return false;
    }
    if (cfg.player_count > 0) {
        const usize roster_bytes =
            static_cast<usize>(cfg.player_count) * sizeof(DemoRosterEntry);
        if (!fwrite_all(f, cfg.roster, roster_bytes)) {
            std::fclose(f);
            return false;
        }
    }

    file_              = f;
    ticks_written_     = 0;
    keyframe_interval_ = (cfg.keyframe_interval == 0) ? 16u : cfg.keyframe_interval;
    start_tick_        = 0;
    last_tick_         = 0;
    last_keyframe_tick_ = 0;
    baseline_.clear();
    toc_.clear();
    scratch_.clear();
    return true;
}

bool DemoWriter::write_frame(u32                 tick,
                              u32                 baseline_tick,
                              std::span<const u8> snapshot_bytes) noexcept {
    if (!file_) return false;
    std::FILE* f = as_file(file_);

    // Decide whether this tick is a keyframe. Force one if:
    //   - first frame ever (baseline_ is empty), OR
    //   - caller asked for tick == baseline_tick, OR
    //   - keyframe interval elapsed since last keyframe.
    const bool force_keyframe = baseline_.empty()
                              || baseline_tick == tick
                              || (tick - last_keyframe_tick_) >= keyframe_interval_;

    // Record this tick's file offset BEFORE writing — needed for the TOC.
    const long here = std::ftell(f);
    if (here < 0) return false;

    scratch_.clear();
    u32 baseline_field;
    if (force_keyframe) {
        // Emit raw snapshot as the payload, set baseline_tick == tick.
        baseline_field = tick;
        scratch_.assign(snapshot_bytes.begin(), snapshot_bytes.end());
    } else {
        baseline_field = last_keyframe_tick_;
        xor_delta_encode(std::span<const u8>(baseline_.data(), baseline_.size()),
                         snapshot_bytes,
                         scratch_);
    }

    // Payload: DemoFrameRecordPreamble (8 bytes) + scratch_.
    DemoRecordHeader rhdr{};
    rhdr.type         = static_cast<u8>(kRecordTypeFrame);
    rhdr.payload_len  = static_cast<u16>(sizeof(DemoFrameRecordPreamble) + scratch_.size());
    rhdr.tick         = tick;
    DemoFrameRecordPreamble pre{};
    pre.baseline_tick = baseline_field;
    pre.entity_count  = 0;  // gameplay schema unknown; bytes are opaque.

    if (!fwrite_all(f, &rhdr, sizeof(rhdr)))  return false;
    if (!fwrite_all(f, &pre,  sizeof(pre)))   return false;
    if (!scratch_.empty()) {
        if (!fwrite_all(f, scratch_.data(), scratch_.size())) return false;
    }

    if (force_keyframe) {
        baseline_.assign(snapshot_bytes.begin(), snapshot_bytes.end());
        last_keyframe_tick_ = tick;
        DemoTocEntry te{};
        te.tick        = tick;
        te.file_offset = static_cast<u32>(here);
        toc_.push_back(te);
    }
    if (start_tick_ == 0 || ticks_written_ == 0) start_tick_ = tick;
    last_tick_ = tick;
    ++ticks_written_;
    return true;
}

bool DemoWriter::write_inputs(u32                               tick,
                               std::span<const PlayerInputEntry> inputs) noexcept {
    if (!file_) return false;
    std::FILE* f = as_file(file_);

    DemoRecordHeader rhdr{};
    rhdr.type        = static_cast<u8>(kRecordTypeInput);
    rhdr.payload_len = static_cast<u16>(sizeof(DemoInputPreamble)
                                         + inputs.size() * sizeof(PlayerInputEntry));
    rhdr.tick        = tick;
    DemoInputPreamble pre{};
    pre.input_count  = static_cast<u8>(inputs.size());

    if (!fwrite_all(f, &rhdr, sizeof(rhdr))) return false;
    if (!fwrite_all(f, &pre,  sizeof(pre)))  return false;
    if (!inputs.empty()) {
        if (!fwrite_all(f, inputs.data(),
                        inputs.size() * sizeof(PlayerInputEntry))) {
            return false;
        }
    }
    return true;
}

void DemoWriter::finalise() noexcept {
    if (!file_) return;
    std::FILE* f = as_file(file_);

    // Serialise TOC + footer at file end.
    const long toc_offset = std::ftell(f);
    if (toc_offset >= 0 && !toc_.empty()) {
        std::fwrite(toc_.data(), sizeof(DemoTocEntry), toc_.size(), f);
    }
    DemoTocFooter footer{};
    footer.toc_offset   = static_cast<u64>(toc_offset < 0 ? 0 : toc_offset);
    footer.toc_count    = static_cast<u32>(toc_.size());
    footer.footer_magic = kDemoMagic;
    std::fwrite(&footer, sizeof(footer), 1, f);

    // Patch header.end_tick + start_tick.
    if (std::fseek(f, 0, SEEK_SET) == 0) {
        DemoFileHeader hdr{};
        if (std::fread(&hdr, sizeof(hdr), 1, f) == 1) {
            hdr.start_tick = start_tick_;
            hdr.end_tick   = last_tick_;
            std::fseek(f, 0, SEEK_SET);
            std::fwrite(&hdr, sizeof(hdr), 1, f);
        }
    }

    std::fflush(f);
    std::fclose(f);
    file_ = nullptr;
}

void DemoWriter::close_unfinished() noexcept {
    if (!file_) return;
    std::fflush(as_file(file_));
    std::fclose(as_file(file_));
    file_ = nullptr;
}

}  // namespace psynder::net
